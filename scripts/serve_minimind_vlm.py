#!/usr/bin/env python3
"""OpenAI-compatible inference bridge for a CellScope-trained MiniMind-V model.

CellScope's llama.cpp backend speaks the OpenAI chat-completions format with
base64 image_url parts. This server accepts that format and runs the PyTorch
MiniMind-V checkpoint produced by:

    cellscope --train ./dataset --train-out ./training

Run from anywhere:

    python scripts/serve_minimind_vlm.py \
        --minimind third_party/minimind-v \
        --save_dir training \
        --weight cellscope_meter \
        --port 8090

Then:

    cellscope --vlm llama --vlm-url http://127.0.0.1:8090
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

MODEL_ID = "minimind-meter"


def parse_args():
    here = os.path.dirname(os.path.abspath(__file__))
    default_mm = os.path.abspath(os.path.join(here, "..", "third_party", "minimind-v"))
    ap = argparse.ArgumentParser()
    ap.add_argument("--minimind", default=default_mm, help="MiniMind-V checkout")
    ap.add_argument("--save_dir", default="", help="directory containing the .pth weights")
    ap.add_argument("--weight", default="cellscope_meter", help="weight prefix")
    ap.add_argument("--hidden_size", type=int, default=768)
    ap.add_argument("--num_hidden_layers", type=int, default=8)
    ap.add_argument("--use_moe", type=int, default=0, choices=[0, 1])
    ap.add_argument("--device", default="", help="cuda:0, mps or cpu (default auto)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8090)
    ap.add_argument("--max_new_tokens", type=int, default=96)
    ap.add_argument("--temperature", type=float, default=0.0)
    return ap.parse_args()


def pick_device(name):
    import torch

    if name:
        return name
    if torch.cuda.is_available():
        return "cuda:0"
    if getattr(torch.backends, "mps", None) and torch.backends.mps.is_available():
        return "mps"
    return "cpu"


def load_model(args):
    import torch
    from transformers import AutoTokenizer

    minimind = os.path.abspath(args.minimind)
    if not os.path.isdir(minimind):
        raise SystemExit(f"MiniMind-V checkout not found: {minimind}")
    os.chdir(minimind)
    sys.path.insert(0, minimind)

    from model.model_vlm import MiniMindVLM, VLMConfig

    save_dir = os.path.abspath(args.save_dir) if args.save_dir else os.path.join(minimind, "out")
    suffix = "_moe" if args.use_moe else ""
    weight_path = os.path.join(save_dir, f"{args.weight}_{args.hidden_size}{suffix}.pth")
    if not os.path.isfile(weight_path):
        raise SystemExit(f"Weights not found: {weight_path}")

    device = pick_device(args.device)
    tokenizer = AutoTokenizer.from_pretrained("./model", trust_remote_code=True)
    config = VLMConfig(
        hidden_size=args.hidden_size,
        num_hidden_layers=args.num_hidden_layers,
        use_moe=bool(args.use_moe),
    )
    model = MiniMindVLM(config, vision_model_path="./model/siglip2-base-p32-256-ve")
    state = torch.load(weight_path, map_location="cpu")
    model.load_state_dict({k: v for k, v in state.items() if "mask" not in k}, strict=False)
    model = model.eval().to(device)
    if device.startswith("cuda"):
        model = model.half()
    print(f"[cellscope-minimind] loaded {weight_path}")
    print(f"[cellscope-minimind] device={device} image_tokens={config.image_token_len}")
    return model, tokenizer, model.processor, device, config


def extract_content(body):
    messages = body.get("messages", [])
    text = ""
    image_b64 = None
    for msg in reversed(messages):
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

    raw = base64.b64decode(image_b64)
    return Image.open(io.BytesIO(raw)).convert("RGB")


def make_handler(model, tokenizer, preprocess, device, config, lock, args):
    import torch
    from model.model_vlm import MiniMindVLM

    class Handler(BaseHTTPRequestHandler):
        server_version = "CellScopeMiniMind/0.1"

        def log_message(self, fmt, *args_):
            sys.stderr.write("[http] " + fmt % args_ + "\n")

        def _json(self, code, payload):
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path.rstrip("/") in ("/v1/models", "/models"):
                self._json(
                    200,
                    {
                        "object": "list",
                        "data": [
                            {
                                "id": MODEL_ID,
                                "object": "model",
                                "owned_by": "cellscope",
                            }
                        ],
                    },
                )
            elif self.path.rstrip("/") in ("/health", "/"):
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
                image_tokens = config.image_special_token * config.image_token_len
                prompt = image_tokens + "\n" + (text or "Read the display.")
                messages = [{"role": "user", "content": prompt}]
                formatted = tokenizer.apply_chat_template(
                    messages, tokenize=False, add_generation_prompt=True,
                    open_thinking=False,
                )
                inputs = tokenizer(
                    formatted,
                    return_tensors="pt",
                    truncation=True,
                    max_length=config.max_seq_len,
                ).to(device)
                pixel_values = {
                    k: v.to(device) for k, v in MiniMindVLM.image2tensor(image, preprocess).items()
                }
                started = time.time()
                with lock, torch.inference_mode():
                    generated = model.generate(
                        inputs.input_ids,
                        attention_mask=inputs.attention_mask,
                        max_new_tokens=args.max_new_tokens,
                        do_sample=args.temperature > 0,
                        temperature=max(args.temperature, 0.01),
                        top_p=1.0,
                        pad_token_id=tokenizer.pad_token_id,
                        eos_token_id=tokenizer.eos_token_id,
                        pixel_values=pixel_values,
                    )
                new_tokens = generated[0, inputs.input_ids.shape[1] :]
                answer = tokenizer.decode(new_tokens, skip_special_tokens=True).strip()
                elapsed = time.time() - started
                print(f"[cellscope-minimind] {elapsed:.2f}s: {answer[:120]!r}")
                self._json(
                    200,
                    {
                        "id": f"chatcmpl-{int(time.time() * 1000)}",
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
                        "usage": {},
                    },
                )
            except Exception as exc:
                import traceback

                traceback.print_exc()
                self._json(500, {"error": {"message": str(exc), "type": "server_error"}})

    return Handler


def main():
    args = parse_args()
    model, tokenizer, preprocess, device, config = load_model(args)
    lock = threading.Lock()
    handler = make_handler(model, tokenizer, preprocess, device, config, lock, args)
    httpd = ThreadingHTTPServer((args.host, args.port), handler)
    print(f"[cellscope-minimind] listening on http://{args.host}:{args.port}/v1")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopping")
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
