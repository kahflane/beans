#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

echo "checking lsp server: lifecycle, overlay, diagnostics"

python3 - "$PWD/build/beansc" <<'PY'
import subprocess, json, re, sys

bin = sys.argv[1]

def frame(o):
    b = json.dumps(o).encode()
    return b"Content-Length: %d\r\n\r\n%b" % (len(b), b)

def run(msgs):
    p = subprocess.run([bin, "lsp"], input=b"".join(msgs),
                       capture_output=True, timeout=20)
    out = p.stdout.decode(errors="replace")
    objs = [json.loads(b) for b in
            re.findall(r"\r\n\r\n(\{.*?\})(?=Content-Length|\Z)", out, re.S)]
    return p.returncode, objs

bad = 'fn main() {\n    let x: int = nope\n}\n'
good = 'fn main() {\n    let x: int = 1\n}\n'
uri = "file:///tmp/beans_lsp_test.b"

rc, objs = run([
    frame({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}),
    frame({"jsonrpc":"2.0","method":"initialized","params":{}}),
    frame({"jsonrpc":"2.0","method":"textDocument/didOpen",
           "params":{"textDocument":{"uri":uri,"text":bad}}}),
    frame({"jsonrpc":"2.0","method":"textDocument/didChange",
           "params":{"textDocument":{"uri":uri},"contentChanges":[{"text":good}]}}),
    # hover the `main` name on the (now valid) buffer: line 0, char 3
    frame({"jsonrpc":"2.0","id":3,"method":"textDocument/hover",
           "params":{"textDocument":{"uri":uri},"position":{"line":0,"character":3}}}),
    frame({"jsonrpc":"2.0","id":2,"method":"shutdown"}),
    frame({"jsonrpc":"2.0","method":"exit"}),
])

def fail(m):
    print("FAIL:", m, file=sys.stderr); sys.exit(1)

if rc != 0:
    fail(f"exit code {rc}, expected 0")

init = next((o for o in objs if o.get("id") == 1), None)
if not init or "capabilities" not in init.get("result", {}):
    fail("initialize did not return capabilities")

pubs = [o for o in objs if o.get("method") == "textDocument/publishDiagnostics"]
if len(pubs) < 2:
    fail(f"expected >=2 diagnostics publishes, got {len(pubs)}")
if len(pubs[0]["params"]["diagnostics"]) != 1:
    fail("open of broken buffer should report exactly one diagnostic")
if "nope" not in pubs[0]["params"]["diagnostics"][0]["message"]:
    fail("diagnostic should mention the unknown name")
if len(pubs[-1]["params"]["diagnostics"]) != 0:
    fail("diagnostics should clear after the fix")

hov = next((o for o in objs if o.get("id") == 3), None)
val = hov and hov.get("result") and hov["result"].get("contents", {}).get("value", "")
if not val or "fn main" not in val:
    fail(f"hover on `main` should render its signature, got: {val!r}")

sh = next((o for o in objs if o.get("id") == 2), None)
if not sh or "result" not in sh:
    fail("shutdown did not reply")

print("ok lsp server: initialize, overlay check, diagnostics, hover")
PY
