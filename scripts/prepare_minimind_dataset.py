#!/usr/bin/env python3
"""Build a MiniMind-V SFT parquet from CellScope's normalized manifest.

The manifest is produced by `cellscope --train DIR` and has columns:
    image,reading,unit,hz

MiniMind-V's VLMDataset expects two columns:
    conversations : JSON string [{"role": "user", ...}, {"role": "assistant", ...}]
    image_bytes   : raw encoded image bytes
"""
import argparse
import csv
import io
import json
import os
import random
import sys

DEFAULT_PROMPT = (
    "You are an optical reader for a bench frequency meter. Look at the attached "
    "camera frame. A reading is usable only when a numeric frequency display is "
    "clearly visible and in focus. Read the digits exactly as shown; never invent "
    "or autocorrect digits. Determine the unit printed next to the display (Hz, "
    "kHz, MHz or GHz). Respond with JSON only, no markdown, in exactly this form:\n"
    '{"visible":true,"reading":"123.456789","unit":"MHz","confidence":0.93}\n'
    "If no display is readable, or it is blurred, cropped, angled or ambiguous, "
    'respond {"visible":false,"reading":"","unit":"","confidence":0.0}. '
    "The reading string must contain digits and at most one decimal separator."
)


def load_prompt(path):
    if path and os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as f:
            return f.read().strip()
    return DEFAULT_PROMPT


def target_json(reading, unit):
    return json.dumps(
        {
            "visible": True,
            "reading": str(reading).strip(),
            "unit": str(unit).strip(),
            "confidence": 0.95,
        },
        separators=(",", ":"),
        ensure_ascii=False,
    )


def conversations_json(prompt, target):
    return json.dumps(
        [
            {"role": "user", "content": "<image>\n" + prompt},
            {"role": "assistant", "content": target},
        ],
        separators=(",", ":"),
        ensure_ascii=False,
    )


def augment_image(image, rng):
    """Simulate a pocket meter held up to a webcam: small rotations, distance,
    off-centre framing, glare and focus changes. Kept moderate so the digits
    remain readable (the target is still the positive reading)."""
    from PIL import Image, ImageEnhance, ImageFilter

    out = image.convert("RGB")
    if rng.random() < 0.85:
        angle = rng.uniform(-9.0, 9.0)
        out = out.rotate(angle, resample=Image.Resampling.BILINEAR, expand=False,
                         fillcolor=(0, 0, 0))
    if rng.random() < 0.6:
        scale = rng.uniform(0.72, 1.0)
        nw, nh = max(64, int(out.width * scale)), max(64, int(out.height * scale))
        small = out.resize((nw, nh), Image.Resampling.BILINEAR)
        canvas = Image.new("RGB", image.size, (0, 0, 0))
        ox = rng.randint(0, max(0, image.width - nw))
        oy = rng.randint(0, max(0, image.height - nh))
        canvas.paste(small, (ox, oy))
        out = canvas
    if rng.random() < 0.6:
        dx = int(image.width * rng.uniform(-0.08, 0.08))
        dy = int(image.height * rng.uniform(-0.08, 0.08))
        canvas = Image.new("RGB", image.size, (0, 0, 0))
        canvas.paste(out, (dx, dy))
        out = canvas
    if rng.random() < 0.7:
        out = ImageEnhance.Brightness(out).enhance(rng.uniform(0.78, 1.2))
    if rng.random() < 0.7:
        out = ImageEnhance.Contrast(out).enhance(rng.uniform(0.8, 1.2))
    if rng.random() < 0.3:
        out = out.filter(ImageFilter.GaussianBlur(radius=rng.uniform(0.3, 1.2)))
    if rng.random() < 0.2:
        from PIL import ImageDraw
        glare = Image.new("RGBA", out.size, (255, 255, 255, 0))
        draw = ImageDraw.Draw(glare)
        rw = rng.randint(out.width // 8, out.width // 3)
        rh = rng.randint(out.height // 10, out.height // 3)
        cx = rng.randint(0, out.width)
        cy = rng.randint(0, out.height)
        draw.ellipse((cx - rw, cy - rh, cx + rw, cy + rh),
                     fill=(255, 255, 255, rng.randint(30, 85)))
        out = Image.alpha_composite(out.convert("RGBA"), glare).convert("RGB")
    buf = io.BytesIO()
    out.save(buf, format="JPEG", quality=90)
    return buf.getvalue()


def main():
    ap = argparse.ArgumentParser(description="Prepare MiniMind-V parquet for CellScope")
    ap.add_argument("--manifest", required=True, help="normalized image,reading,unit,hz CSV")
    ap.add_argument("--out", required=True, help="destination .parquet")
    ap.add_argument("--prompt-file", default="", help="prompt text written by CellScope")
    ap.add_argument("--augment", type=int, default=0, help="extra augmented variants per image")
    ap.add_argument("--seed", type=int, default=2026)
    ap.add_argument("--limit", type=int, default=0, help="optional row limit for a smoke test")
    args = ap.parse_args()

    try:
        import pyarrow as pa
        import pyarrow.parquet as pq
        from PIL import Image
    except Exception as exc:  # pragma: no cover - user environment issue
        print("Missing Python package:", exc, file=sys.stderr)
        print("Install with: python -m pip install pillow pyarrow", file=sys.stderr)
        return 2

    prompt = load_prompt(args.prompt_file)
    rows = []
    with open(args.manifest, "r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        needed = {"image", "reading", "unit", "hz"}
        if not needed.issubset(set(reader.fieldnames or [])):
            print("Manifest must contain image,reading,unit,hz", file=sys.stderr)
            return 2
        for i, row in enumerate(reader):
            if args.limit and i >= args.limit:
                break
            path = row["image"]
            if not os.path.isfile(path):
                print("Skipping missing image:", path, file=sys.stderr)
                continue
            try:
                with open(path, "rb") as imf:
                    raw = imf.read()
                image = Image.open(io.BytesIO(raw)).convert("RGB")
            except Exception as exc:
                print("Skipping unreadable image:", path, exc, file=sys.stderr)
                continue
            target = target_json(row["reading"], row["unit"])
            conv = conversations_json(prompt, target)
            rows.append((conv, raw, path, row.get("hz", "")))
            rng = random.Random(args.seed + i)
            for _ in range(max(0, args.augment)):
                try:
                    rows.append((conv, augment_image(image, rng), path, row.get("hz", "")))
                except Exception as exc:
                    print("Augmentation failed:", path, exc, file=sys.stderr)

    if not rows:
        print("No usable rows; refusing to write an empty dataset.", file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    schema = pa.schema([("conversations", pa.string()), ("image_bytes", pa.binary())])
    batch_size = 64
    with pq.ParquetWriter(args.out, schema, compression="zstd") as writer:
        for start in range(0, len(rows), batch_size):
            chunk = rows[start : start + batch_size]
            table = pa.Table.from_pydict(
                {
                    "conversations": [r[0] for r in chunk],
                    "image_bytes": [r[1] for r in chunk],
                },
                schema=schema,
            )
            writer.write_table(table)

    unique = len({r[2] for r in rows})
    print(
        f"Wrote {len(rows)} examples ({unique} unique images, augment={args.augment}) "
        f"to {args.out}"
    )
    print("Dataset is ready for MiniMind-V train_sft_vlm.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
