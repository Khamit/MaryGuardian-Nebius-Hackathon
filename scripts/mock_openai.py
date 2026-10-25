#!/usr/bin/env python3
"""Мок OpenAI-совместимого API (для тестов и CI без кредитов).

Каждый полученный запрос дописывается в $MOCK_LOG (jsonl) — по нему тест проверяет,
что в «облако» не уехало ни одного сырого PII. Поведение выбирается маркерами в user-сообщении:

  MODE:429        первый запрос → HTTP 429, повтор → успех
  MODE:REASONING  при max_tokens < 4000 → content=null, finish_reason=length (как у reasoning-моделей)
  MODE:JSONFMT    если в теле есть response_format → HTTP 400 про response_format
  MODE:FIELDS     JSON с полями документа (все корректные)
  MODE:FIELDS_BAD JSON с выдуманными/некорректными полями
  ECHO            вернуть user-текст как есть (проверка обратной подстановки)
  иначе           JSON-ответ классификатора, в который подставлены заглушки из запроса
"""
import json
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

LOG = os.environ.get("MOCK_LOG", "/tmp/mock_requests.jsonl")
seen = set()


def first(pattern, text, default):
    m = re.search(pattern, text)
    return m.group(0) if m else default


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, obj):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path.endswith("/models"):
            ids = ["nvidia/NVIDIA-Nemotron-3-Nano-30B-A3B", "nvidia/nemotron-3-super-120b-a12b",
                   "nvidia/Nemotron-3-Ultra-550b-a55b"]
            return self._send(200, {"data": [{"id": i} for i in ids]})
        self._send(404, {"error": "not found"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n).decode()
        body = json.loads(raw)
        with open(LOG, "a") as f:
            f.write(json.dumps({"auth": self.headers.get("Authorization", ""), "body": body}) + "\n")
        user = body["messages"][-1]["content"]

        if "MODE:429" in user and "429" not in seen:
            seen.add("429")
            return self._send(429, {"error": {"message": "rate limited"}})
        if "MODE:JSONFMT" in user and "response_format" in body:
            return self._send(400, {"error": {"message": "response_format is not supported"}})
        if "MODE:REASONING" in user and body.get("max_tokens", 0) < 4000:
            return self._send(200, {"choices": [{"message": {"content": None, "reasoning_content": "thinking..."},
                                                 "finish_reason": "length"}],
                                    "usage": {"prompt_tokens": 50, "completion_tokens": body["max_tokens"]}})
        if "MODE:FIELDS_BAD" in user:      # выдуманные значения: код обязан их отклонить
            c = {"doc_type": "invoice", "doc_date": "2026-02-30", "doc_number": "XYZ-999", "total_amount": 999999,
                 "currency": "EUR", "issuer": "Made Up Corp", "valid_until": "12/04/2026"}
            return self._send(200, {"choices": [{"message": {"content": json.dumps(c)}, "finish_reason": "stop"}],
                                    "usage": {"prompt_tokens": 200, "completion_tokens": 60}})
        if "MODE:FIELDS" in user:
            c = {"doc_type": "invoice", "doc_date": "2026-03-12", "doc_number": "INV-2026-0042", "total_amount": 1250,
                 "currency": "USD", "issuer": "Acme Supplies Ltd", "valid_until": "2026-04-11"}
            return self._send(200, {"choices": [{"message": {"content": json.dumps(c)}, "finish_reason": "stop"}],
                                    "usage": {"prompt_tokens": 200, "completion_tokens": 60}})
        if "ECHO" in user:
            content = user
        else:
            person = first(r"\[PERSON_\d+\]", user, "ACME Ltd")
            iban = first(r"\[IBAN_\d+\]", user, "n/a")
            content = "<think>hidden</think>" + json.dumps(
                {"doc_type": "invoice", "company": person, "summary": f"Invoice paid to {iban}"})
        self._send(200, {"choices": [{"message": {"content": content}, "finish_reason": "stop"}],
                         "usage": {"prompt_tokens": 120, "completion_tokens": 40}})


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
    print(f"mock OpenAI API on http://127.0.0.1:{port}/v1", flush=True)
    HTTPServer(("127.0.0.1", port), H).serve_forever()
