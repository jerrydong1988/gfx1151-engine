"""Opt-in real-model image capacity/ordering check; never starts a server.

Requires Pillow and requests. Fixtures contain independent visible codes;
expected answers are never included in the prompt. Use a dedicated test API
to avoid adding synthetic requests to production statistics.
"""
import argparse
import base64
import io
import json
import random
import re
import time
from pathlib import Path

import requests
from PIL import Image, ImageDraw, ImageFont


def fixtures(count=32, width=640, height=384):
    font = None
    for name in ("C:/Windows/Fonts/arialbd.ttf", "DejaVuSans-Bold.ttf"):
        try:
            font = ImageFont.truetype(name, max(24, height // 4))
            break
        except OSError:
            pass
    if font is None:
        raise RuntimeError("A large readable TrueType font is required")
    codes = random.Random(10732).sample(range(100, 1000), count)
    urls = []
    for i, code in enumerate(codes):
        image = Image.new("RGB", (width, height), "white")
        draw = ImageDraw.Draw(image)
        draw.rectangle((0, 0, width-1, height-1), outline=(32, 64, 96), width=8)
        draw.text((width // 2, height // 2), str(code), font=font, fill="black", anchor="mm")
        buffer = io.BytesIO()
        image.save(buffer, format="PNG")
        urls.append("data:image/png;base64," + base64.b64encode(buffer.getvalue()).decode())
    return codes, urls


def messages(urls, tag, all_codes=False):
    n = len(urls)
    positions = list(range(n)) if all_codes else sorted({0, n // 2, n-1})
    instruction = ("Read the three-digit code printed in every image, in image order."
                   if all_codes else
                   "Read the three-digit code printed in image positions " +
                   ", ".join(str(i+1) for i in positions) + " (1-based).")
    text = (f"Check {tag}. {instruction} Each three-digit code is ONE integer, "
            f"not three separate digits. Return only a JSON array containing exactly {len(positions)} integers.")
    content = [{"type": "text", "text": text}]
    content += [{"type": "image_url", "image_url": {"url": url}} for url in urls]
    return [{"role": "user", "content": content}], positions


def chat(base, msg, max_tokens=256):
    body = dict(messages=msg, temperature=0, enable_thinking=False, max_tokens=max_tokens)
    start = time.perf_counter()
    response = requests.post(base + "/v1/chat/completions", json=body, timeout=900)
    elapsed = time.perf_counter() - start
    result = dict(status=response.status_code, wall_s=elapsed, body=response.json())
    if response.status_code == 200:
        result["answer"] = result["body"]["choices"][0]["message"].get("content", "")
        result["codes"] = list(map(int, re.findall(r"\b\d{3}\b", result["answer"])))
        tail = requests.get(base + "/reqstat/tail?n=1", timeout=10).json().get("records", [])
        result["stats"] = tail[-1] if tail else {}
    return result


def run(base, output, counts=(16, 17, 20, 32)):
    codes, urls = fixtures()
    results = []
    def save():
        output.write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for n in counts:
        msg, positions = messages(urls[:n], f"capacity-{n}", all_codes=True)
        result = chat(base, msg)
        result.update(name=f"all_codes_{n}", images=n, expected=[codes[i] for i in positions])
        result["passed"] = result["status"] == 200 and result.get("codes") == result["expected"]
        results.append(result); save()
        print(result["name"], result["passed"], result.get("answer"), flush=True)
        if not result["passed"]:
            raise AssertionError(result["name"])
    # Actual sequential history: keep the first request's images and its reply,
    # then add enough new images to cross the old cap.
    first, _ = messages(urls[:16], "history-first", all_codes=True)
    reply = chat(base, first)
    assert reply["status"] == 200
    extra, _ = messages(urls[16:20], "history-new-only", all_codes=True)
    extra[0]["content"][0]["text"] = (
        "Read the codes ONLY in the four newly attached images in this last message. "
        "Each three-digit code is ONE integer, not separate digits. "
        "Return only a JSON array of those four integers, in attachment order.")
    history = first + [{"role": "assistant", "content": reply["answer"]}] + extra
    result = chat(base, history)
    result.update(name="history_16_plus_4", images=20, expected=codes[16:20])
    result["passed"] = result["status"] == 200 and result.get("codes") == result["expected"]
    results.append(result); save()
    assert result["passed"], result
    bad, _ = messages(urls + urls[:1], "over-limit")
    result = chat(base, bad)
    result.update(name="reject_33", passed=result["status"] == 400 and
                  "at most 32" in json.dumps(result["body"]))
    results.append(result); save(); assert result["passed"], result
    result = chat(base, [{"role": "user", "content": "Reply only RECOVERED."}])
    result.update(name="recovery_after_reject", passed=result["status"] == 200 and
                  result.get("answer", "").strip() == "RECOVERED")
    results.append(result); save(); assert result["passed"], result
    print("PASS capacity, ordering, history and rejection recovery", flush=True)
    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    run(args.base.rstrip("/"), args.output)
