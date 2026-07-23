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

# signature help: cursor inside a call's argument list
callsrc = ("fn add(a: int, b: int) -> int {\n    return a + b\n}\n"
           "fn main() {\n    let y: int = add(1, 2)\n}\n")
curi = "file:///tmp/beans_lsp_sig.b"
rc2, objs2 = run([
    frame({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}),
    frame({"jsonrpc":"2.0","method":"textDocument/didOpen",
           "params":{"textDocument":{"uri":curi,"text":callsrc}}}),
    # 'add(1, 2)' is on line 4; the first argument sits at character 21
    frame({"jsonrpc":"2.0","id":7,"method":"textDocument/signatureHelp",
           "params":{"textDocument":{"uri":curi},"position":{"line":4,"character":21}}}),
    # go-to-definition on the `add` call (character 17) -> its definition on line 0
    frame({"jsonrpc":"2.0","id":8,"method":"textDocument/definition",
           "params":{"textDocument":{"uri":curi},"position":{"line":4,"character":17}}}),
    frame({"jsonrpc":"2.0","id":10,"method":"textDocument/documentSymbol",
           "params":{"textDocument":{"uri":curi}}}),
    frame({"jsonrpc":"2.0","id":11,"method":"textDocument/semanticTokens/full",
           "params":{"textDocument":{"uri":curi}}}),
    frame({"jsonrpc":"2.0","id":2,"method":"shutdown"}),
    frame({"jsonrpc":"2.0","method":"exit"}),
])
tok = next((o for o in objs2 if o.get("id") == 11), None)
data = tok.get("result", {}).get("data") if tok else None
if not data or len(data) % 5 != 0 or len(data) == 0:
    fail(f"semantic tokens should return a non-empty multiple-of-5 array, got: {data!r}")
# first token is `add` on line 0 -> tokenType 1 (function) in the legend
if data[3] != 1:
    fail(f"first semantic token should be a function (type 1), got type {data[3]}")
dfn = next((o for o in objs2 if o.get("id") == 8), None)
dres = dfn and dfn.get("result")
if not dres or dres.get("range", {}).get("start", {}).get("line") != 0:
    fail(f"definition of `add` should point at line 0, got: {dres!r}")
sym = next((o for o in objs2 if o.get("id") == 10), None)
names = [s["name"] for s in (sym.get("result") or [])] if sym else []
if "add" not in names or "main" not in names:
    fail(f"document symbols should list add and main, got: {names}")
sig = next((o for o in objs2 if o.get("id") == 7), None)
res = sig and sig.get("result")
if not res or not res.get("signatures"):
    fail(f"signature help returned nothing: {sig!r}")
if "add(a: int, b: int)" not in res["signatures"][0]["label"]:
    fail(f"unexpected signature label: {res['signatures'][0]['label']!r}")
if res.get("activeParameter") != 0:
    fail(f"expected activeParameter 0, got {res.get('activeParameter')}")

# completion: members after `.` on a half-typed line
compsrc = ("class Point {\n    x: int\n    fn norm2() -> int { return self.x }\n}\n"
           "fn main() {\n    let p: Point = new Point(1, 2)\n    p.\n}\n")
kuri = "file:///tmp/beans_lsp_comp.b"
rc3, objs3 = run([
    frame({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}),
    frame({"jsonrpc":"2.0","method":"textDocument/didOpen",
           "params":{"textDocument":{"uri":kuri,"text":compsrc}}}),
    # 'p.' is on line 6; the cursor sits just after the dot at character 6
    frame({"jsonrpc":"2.0","id":9,"method":"textDocument/completion",
           "params":{"textDocument":{"uri":kuri},"position":{"line":6,"character":6}}}),
    frame({"jsonrpc":"2.0","id":2,"method":"shutdown"}),
    frame({"jsonrpc":"2.0","method":"exit"}),
])
comp = next((o for o in objs3 if o.get("id") == 9), None)
cres = comp and comp.get("result")
labels = [i["label"] for i in cres.get("items", [])] if cres else []
if "norm2" not in labels or "x" not in labels:
    fail(f"member completion after p. should include x and norm2, got: {labels}")

print("ok lsp server: init, overlay, diagnostics, hover, signature, "
      "completion, definition, symbols, semantic tokens")
PY
