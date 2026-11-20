#!/usr/bin/env python3
"""OpenAI-compatible local VLM bridge for a Hugging Face Transformers model.

CellScope launches this with its private Python runtime when settings.json
points at a downloaded Hugging Face model:

    python scripts/serve_hf_vlm.py --model /path/to/snapshot --port 8091

The HTTP contract matches the existing reader: POST /v1/chat/completions with
an image_url data: part and a text prompt; GET /health and /v1/models.
"""
import argparse
import base64
import io
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODEL_ID = "cellscope-hf"


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True, help="local Hugging Face snapshot directory")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8091)
    ap.add_argument("--device", default="auto", help="auto, cpu, mps, cuda:0 ...")
    ap.add_argument("--dtype", default="auto", help="auto, float32, float16, bfloat16")
    ap.add_argument("--max_new_tokens", type=int, default=128)
    ap.add_argument("--trust_remote_code", type=int, default=1, choices=[0, 1])
    return ap.parse_args()


def pick_device(name):
    import torch

    if name and name != "auto":
        return name
    if torch.cuda.is_available():
        return "cuda:0"
    if getattr(torch.backends, "mps", None) and torch.backends.mps.is_available():
        return "mps"
    return "cpu"


def pick_dtype(name, device):
    import torch

    if name == "float16":
        return torch.float16
    if name == "bfloat16":
        return torch.bfloat16
    if name == "float32":
        return torch.float32
    if device.startswith("cuda"):
        if torch.cuda.is_bf16_supported():
            return torch.bfloat16
        return torch.float16
    if device == "mps":
        return torch.float16
    return torch.float32


def load_model(args):
    import torch
    from transformers import AutoProcessor
    try:
        from transformers import AutoModelForImageTextToText as AutoModel
    except Exception:
        from transformers import AutoModelForVision2Seq as AutoModel

    model_path = os.path.abspath(args.model)
    if not os.path.isdir(model_path):
        raise SystemExit(f"Model directory not found: {model_path}")
    device = pick_device(args.device)
    dtype = pick_dtype(args.dtype, device)
    print(f"[cellscope-hf] loading {model_path} device={device} dtype={dtype}")
    started = time.time()
    processor = AutoProcessor.from_pretrained(
        model_path, trust_remote_code=bool(args.trust_remote_code)
    )
    model = AutoModel.from_pretrained(
        model_path,
        torch_dtype=dtype,
        low_cpu_mem_usage=True,
        trust_remote_code=bool(args.trust_remote_code),
    )
    model = model.eval().to(device)
    print(f"[cellscope-hf] loaded in {time.time() - started:.1f}s")
    return model, processor, device, dtype


def extract_content(body):
    text = ""
    image_b64 = None
    for msg in reversed(body.get("messages", [])):
        if msg.get("role") != "user":
            continue
        content = msg.get("content", "")
        if isinstance(content, str):
            text = content
        elif isinstance(content, list):
            parts = []
            for part in content:
                if not isinstance(part, dict):
                    continue
                if part.get("type") == "text":
                    parts.append(part.get("text", ""))
                elif part.get("type") == "image_url":
                    url = part.get("image_url", {})
                    if isinstance(url, dict):
                        url = url.get("url", "")
                    if isinstance(url, str) and url.startswith("data:"):
                        image_b64 = url.split(",", 1)[1] if "," in url else None
            text = "\n".join(parts)
        break
    return text, image_b64


def decode_image(image_b64):
    from PIL import Image

    return Image.open(io.BytesIO(base64.b64decode(image_b64))).convert("RGB")


def build_prompt(processor, text):
    messages = [
        {
            "role": "user",
            "content": [
                {"type": "image"},
                {"type": "text", "text": text or "Read the frequency display."},
            ],
        }
    ]
    try:
        return processor.apply_chat_template(
            messages, tokenize=False, add_generation_prompt=True
        )
    except Exception:
        return "<image>\n" + (text or "Read the frequency display.")


def make_handler(model, processor, device, dtype, lock, args):
    import torch

    class Handler(BaseHTTPRequestHandler):
        server_version = "CellScopeHF/0.1"

        def log_message(self, fmt, *values):
            sys.stderr.write("[http] " + fmt % values + "\n")

        def _json(self, code, payload):
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            path = self.path.rstrip("/")
            if path in ("/v1/models", "/models"):
                self._json(
                    200,
                    {
                        "object": "list",
                        "data": [
                            {"id": MODEL_ID, "object": "model", "owned_by": "cellscope"}
                        ],
                    },
                )
            elif path in ("/health", "/"):
                self._json(200, {"status": "ok", "model": MODEL_ID})
            else:
                self._json(404, {"error": "not found"})

        def do_POST(self):
            if self.path.rstrip("/") != "/v1/chat/completions":
                self._json(404, {"error": "not found"})
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(length) or b"{}")
                text, image_b64 = extract_content(body)
                if not image_b64:
                    raise ValueError("no image_url data part in the request")
                image = decode_image(image_b64)
                prompt = build_prompt(processor, text)
                inputs = processor(text=[prompt], images=[image], return_tensors="pt")
                values = {}
                for key, value in inputs.items():
                    if torch.is_tensor(value):
                        if value.is_floating_point():
                            value = value.to(device=device, dtype=dtype)
                        else:
                            value = value.to(device)
                    values[key] = value
                started = time.time()
                # Some processors return helper fields that a particular model's
                # generate() does not accept (e.g. batch_num_images). Keep the
                # common multimodal generation inputs only.
                allowed = {
                    "input_ids", "attention_mask", "pixel_values",
                    "pixel_attention_mask", "pixel_values_videos", "image_sizes",
                    "image_grid_thw",
                    "token_type_ids", "mm_token_type_ids", "aspect_ratio_ids",
                    "aspect_ratio_mask", "cross_attention_mask",
                }
                gen_values = {k: v for k, v in values.items() if k in allowed}
                tokenizer = getattr(processor, "tokenizer", None)
                gen = {"max_new_tokens": max(1, args.max_new_tokens), "do_sample": False}
                pad = getattr(tokenizer, "pad_token_id", None)
                eos = getattr(tokenizer, "eos_token_id", None)
                if pad is not None:
                    gen["pad_token_id"] = pad
                if eos is not None:
                    gen["eos_token_id"] = eos
                with lock, torch.inference_mode():
                    generated = model.generate(**gen_values, **gen)
                input_ids = gen_values.get("input_ids")
                trim = int(input_ids.shape[1]) if input_ids is not None else 0
                answer = processor.batch_decode(
                    generated[:, trim:], skip_special_tokens=True
                )[0].strip()
                print(f"[cellscope-hf] {time.time() - started:.2f}s: {answer[:160]!r}",
                      flush=True)
                self._json(
                    200,
                    {
                        "id": "chatcmpl-cellscope",
                        "object": "chat.completion",
                        "created": int(time.time()),
                        "model": MODEL_ID,
                        "choices": [
                            {
                                "index": 0,
                                "message": {"role": "assistant", "content": answer},
                                "finish_reason": "stop",
                            }
                        ],
                    },
                )
            except Exception as exc:
                print("[cellscope-hf] error:", exc, file=sys.stderr)
                self._json(500, {"error": str(exc)})

    return Handler


def main():
    args = parse_args()
    if not 1 <= args.port <= 65535:
        raise SystemExit("invalid port")
    model, processor, device, dtype = load_model(args)
    lock = threading.Lock()
    handler = make_handler(model, processor, device, dtype, lock, args)
    server = ThreadingHTTPServer((args.host, args.port), handler)
    print(f"[cellscope-hf] listening on http://{args.host}:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
