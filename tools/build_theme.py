#!/usr/bin/env python3
"""将 theme/tokens.json 注入 theme/theme.template.qss，生成 resources/theme.qss。

用法: python tools/build_theme.py
"""
from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOKENS = ROOT / "theme" / "tokens.json"
TEMPLATE = ROOT / "theme" / "theme.template.qss"
OUTPUT = ROOT / "resources" / "theme.qss"

PLACEHOLDER = re.compile(r"@([a-z]+)-([a-z0-9-]+)")


def flatten(tokens: dict) -> dict[str, str]:
    result: dict[str, str] = {}
    for group, values in tokens.items():
        for key, value in values.items():
            result[f"{group}-{key}"] = value
    return result


def main() -> int:
    tokens = json.loads(TOKENS.read_text(encoding="utf-8"))
    flat = flatten(tokens)
    text = TEMPLATE.read_text(encoding="utf-8")

    missing: set[str] = set()

    def replace(match: re.Match[str]) -> str:
        name = match.group(1) + "-" + match.group(2)
        if name not in flat:
            missing.add(name)
            return match.group(0)
        return flat[name]

    rendered = PLACEHOLDER.sub(replace, text)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(rendered, encoding="utf-8")

    if missing:
        print("缺少令牌:", ", ".join(sorted(missing)))
        return 1
    print(f"生成 {OUTPUT.relative_to(ROOT)} ({len(rendered)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
