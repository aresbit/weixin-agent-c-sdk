#!/usr/bin/env python3
import json
import sys
import uuid


def write(obj):
    sys.stdout.write(json.dumps(obj, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def first_text(prompt):
    for block in prompt:
        if block.get("type") == "text":
            return block.get("text", "")
    return ""


for raw in sys.stdin:
    raw = raw.strip()
    if not raw:
        continue
    msg = json.loads(raw)
    method = msg.get("method")
    msg_id = msg.get("id")
    params = msg.get("params", {})

    if method == "initialize":
      write({
          "jsonrpc": "2.0",
          "id": msg_id,
          "result": {
              "protocolVersion": 1,
              "agentCapabilities": {
                  "loadSession": False,
                  "promptCapabilities": {
                      "image": True,
                      "audio": True,
                      "embeddedContext": True,
                  },
              },
              "agentInfo": {"name": "mock-acp", "version": "0.1.0"},
          },
      })
    elif method == "session/new":
      write({
          "jsonrpc": "2.0",
          "id": msg_id,
          "result": {
              "sessionId": f"sess_{uuid.uuid4().hex[:12]}"
          },
      })
    elif method == "session/prompt":
      session_id = params.get("sessionId")
      prompt = params.get("prompt", [])
      text = first_text(prompt)
      write({
          "jsonrpc": "2.0",
          "method": "session/update",
          "params": {
              "sessionId": session_id,
              "update": {
                  "sessionUpdate": "agent_message_chunk",
                  "content": {
                      "type": "text",
                      "text": f"mock:{text}",
                  },
              },
          },
      })
      write({
          "jsonrpc": "2.0",
          "id": msg_id,
          "result": {"stopReason": "end_turn"},
      })
    else:
      write({
          "jsonrpc": "2.0",
          "id": msg_id,
          "error": {"code": -32601, "message": f"method not found: {method}"},
      })
