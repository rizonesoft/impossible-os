import io
import json

from conclave import http


class _Resp(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def test_chat_parses_content_cost_tokens(monkeypatch):
    body = {"choices": [{"message": {"content": "hi"}}],
            "usage": {"cost": 0.01, "prompt_tokens": 10, "completion_tokens": 5}}
    monkeypatch.setattr(http.urllib.request, "urlopen",
                        lambda req, timeout: _Resp(json.dumps(body).encode()))
    content, cost, sec, toks = http.chat("k", "m", [{"role": "user", "content": "x"}], 5)
    assert content == "hi" and cost == 0.01 and toks == {"prompt": 10, "completion": 5}
    assert sec >= 0


def test_embed_parses_vectors(monkeypatch):
    body = {"data": [{"embedding": [0.1, 0.2]}, {"embedding": [0.3, 0.4]}],
            "usage": {"cost": 0.002}}
    monkeypatch.setattr(http.urllib.request, "urlopen",
                        lambda req, timeout: _Resp(json.dumps(body).encode()))
    vecs, cost = http.embed("k", "e", ["a", "b"], 5)
    assert vecs == [[0.1, 0.2], [0.3, 0.4]] and cost == 0.002


def test_remaining_credits(monkeypatch):
    body = {"data": {"total_credits": 10.0, "total_usage": 3.5}}
    monkeypatch.setattr(http.urllib.request, "urlopen",
                        lambda req, timeout: _Resp(json.dumps(body).encode()))
    assert http.remaining_credits("k") == 6.5
