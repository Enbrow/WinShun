# A once Win顺 plugin in Python: the query comes in WS_* environment
# variables, the items go out as one JSON object on stdout.
import json
import os
import sys

query = os.environ.get("WS_QUERY", "")
items = [
    {"id": "echo", "title": f"回声：{query}", "subtitle": f"PID {os.getpid()} · {os.environ.get('WS_COMMAND')}/{os.environ.get('WS_TRIGGER')}", "score": 80, "copy": query},
    {"id": "upper", "title": query.upper(), "subtitle": "大写 · upper case"},
    {"id": "len", "title": f"{len(query)} 个字符", "subtitle": "length", "valid": False},
]
sys.stdout.write(json.dumps({"items": items}, ensure_ascii=False))
sys.stdout.write("\n")
