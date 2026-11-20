#!/usr/bin/env python3
"""Inspect a local Hugging Face snapshot and emit the CellScope environment spec.

The output is a key=value file because the C runtime consumes it without a JSON
dependency. Requirements are inferred from config.json and the model card,
with conservative defaults when a model card is silent.
"""
import argparse
import json
import os
import re
import sys


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def version_tuple(text):
    m = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", text or "")
    if not m:
        return None
    return tuple(int(x) if x is not None else 0 for x in m.groups())


def choose_python(config, card):
    # Prefer an explicit requirement in the model card.
    patterns = [
        r"(?:python_requires|requires-python|python\s*(?:>=|≥|>))\s*[=:>\s]*([0-9]+\.[0-9]+)",
        r"Python\s+([0-9]+\.[0-9]+)\+",
    ]
    for pat in patterns:
        m = re.search(pat, card or "", re.IGNORECASE)
        if m:
            return m.group(1)
    tv = version_tuple(str(config.get("transformers_version", "")))
    if tv and tv[0] >= 5:
        return "3.11"
    return "3.11"


def choose_transformers(config, card):
    # config.json is written by the Transformers version used to export the model.
    tv = version_tuple(str(config.get("transformers_version", "")))
    if tv:
        if tv[0] >= 5:
            return f">={tv[0]}.{tv[1]},<6"
        if tv[0] == 4:
            return "4.57.6"
    # Model-card hints such as "transformers>=5.3.0".
    m = re.search(r"transformers\s*(?:>=|≥|>)\s*([0-9]+\.[0-9]+(?:\.[0-9]+)?)",
                  card or "", re.IGNORECASE)
    if m:
        v = version_tuple(m.group(1))
        if v and v[0] >= 5:
            return f">={v[0]}.{v[1]},<6"
        return "4.57.6"
    return "4.57.6"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--snapshot", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--repo", default="")
    args = ap.parse_args()
    cfg_path = os.path.join(args.snapshot, "config.json")
    try:
        with open(cfg_path, "r", encoding="utf-8") as f:
            config = json.load(f)
    except Exception as exc:
        print(f"cannot read {cfg_path}: {exc}", file=sys.stderr)
        return 2
    card = read_text(os.path.join(args.snapshot, "README.md"))
    python = choose_python(config, card)
    transformers = choose_transformers(config, card)
    archs = config.get("architectures") or []
    arch = archs[0] if archs else ""
    model_type = config.get("model_type", "")
    lines = [
        "version=1",
        f"repo={args.repo}",
        f"python={python}",
        f"transformers_spec={transformers}",
        f"model_type={model_type}",
        f"architecture={arch}",
        "packages=torch,torchvision,accelerate,safetensors,huggingface_hub,"
        "sentencepiece,protobuf,timm,einops,Pillow,numpy",
        f"reason=config transformers_version={config.get('transformers_version', '')}; "
        f"arch={arch}; model_type={model_type}",
    ]
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
