#!/usr/bin/env python3
"""Download a Hugging Face model snapshot for CellScope's local reader.

The token is read from the environment (HF_TOKEN, HUGGINGFACE_HUB_TOKEN,
HUGGINGFACE_API_KEY or HF_API_KEY) and is never written to disk or printed.
"""
import argparse
import os
import sys


def token_from_env():
    for name in ("HF_TOKEN", "HUGGINGFACE_HUB_TOKEN", "HUGGING_FACE_HUB_TOKEN",
                 "HUGGINGFACEHUB_API_TOKEN", "HUGGINGFACE_API_KEY", "HF_API_KEY",
                 "HF_API_TOKEN"):
        value = os.environ.get(name, "").strip()
        if value:
            return value, name
    return None, None


def main():
    ap = argparse.ArgumentParser(description="Download a Hugging Face model for CellScope")
    ap.add_argument("--repo", required=True, help="Hugging Face repo id, e.g. org/model")
    ap.add_argument("--dest", required=True, help="local snapshot directory")
    ap.add_argument("--revision", default="main")
    ap.add_argument("--max-workers", type=int, default=4)
    args = ap.parse_args()

    token, token_name = token_from_env()
    if token:
        print(f"Using Hugging Face token from {token_name}.")
    else:
        print("No Hugging Face token found; only public/ungated models can be downloaded.",
              file=sys.stderr)

    try:
        from huggingface_hub import snapshot_download
    except Exception as exc:
        print("Missing huggingface_hub. Run CellScope's --setup-runtime first.", file=sys.stderr)
        print(exc, file=sys.stderr)
        return 2

    os.makedirs(args.dest, exist_ok=True)
    print(f"Downloading {args.repo} -> {args.dest}")
    try:
        path = snapshot_download(
            repo_id=args.repo,
            revision=args.revision,
            local_dir=args.dest,
            token=token,
            max_workers=max(1, args.max_workers),
        )
    except Exception as exc:
        message = str(exc)
        if token:
            # Do not echo the token if a library included it in an error.
            message = message.replace(token, "<HF_TOKEN>")
        print("Download failed:", message, file=sys.stderr)
        return 3

    print(f"Snapshot ready: {os.path.abspath(path)}")
    if not os.path.isfile(os.path.join(path, "config.json")):
        print("Warning: config.json is absent; this may not be a Transformers model.",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
