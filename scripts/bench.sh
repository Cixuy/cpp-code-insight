#!/usr/bin/env bash
# 简单压测：复用同一个 TCP 连接连续发请求（keep-alive），
# 用来观察单核吞吐和 P99。正式压测建议用 wrk：
#   wrk -t4 -c200 -d10s --latency -s scripts/scan.lua http://127.0.0.1:18080
set -euo pipefail

HOST=${1:-127.0.0.1}
PORT=${2:-18080}
REQ=${3:-2000}

python - "$HOST" "$PORT" "$REQ" <<'PY'
import socket, sys, time, statistics

host, port, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
body = b"public class Demo {\n  void f() throws Exception {\n    String password = \"hardcoded123\";\n    try { g(); } catch (Exception e) {}\n  }\n}\n"
req = (
    b"POST /scan?filename=Demo.java HTTP/1.1\r\n"
    b"Host: %s\r\n"
    b"Content-Type: text/plain\r\n"
    b"Content-Length: %d\r\n\r\n%s"
) % (host.encode(), len(body), body)

sock = socket.create_connection((host, port))
sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

lat = []
start = time.perf_counter()
for _ in range(n):
    t0 = time.perf_counter()
    sock.sendall(req)
    # 读到 header 结束 + body 即可（这里简化：读到 \r\n\r\n 后再读 Content-Length）
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise SystemExit("connection closed")
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    length = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":")[1])
    while len(rest) < length:
        rest += sock.recv(4096)
    lat.append((time.perf_counter() - t0) * 1000)
elapsed = time.perf_counter() - start
sock.close()

lat.sort()
p50 = lat[int(len(lat) * 0.50)]
p99 = lat[int(len(lat) * 0.99)]
print(f"requests : {n}")
print(f"elapsed  : {elapsed:.3f}s")
print(f"qps      : {n / elapsed:.0f}")
print(f"latency  : p50={p50:.2f}ms  p99={p99:.2f}ms  mean={statistics.mean(lat):.2f}ms")
PY
