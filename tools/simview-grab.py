"""Pull one PNG part out of the sim's multipart stream, for verification."""
import socket
import sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
out = sys.argv[2] if len(sys.argv) > 2 else "/src/out/host/view-frame.png"

s = socket.create_connection(("127.0.0.1", port), timeout=5)
s.sendall(b"GET /stream HTTP/1.0\r\n\r\n")

buf = b""
while len(buf) < 4 * 1024 * 1024:
    chunk = s.recv(65536)
    if not chunk:
        break
    buf += chunk
    start = buf.find(b"\x89PNG")
    if start < 0:
        continue
    end = buf.find(b"IEND", start)
    if end > 0 and len(buf) >= end + 8:
        open(out, "wb").write(buf[start:end + 8])
        print("wrote", out, end + 8 - start, "bytes")
        # How many parts arrived in what we read, as a rate sanity check.
        print("parts seen:", buf.count(b"Content-Type: image/png"))
        s.close()
        raise SystemExit(0)
print("no complete PNG in", len(buf), "bytes")
raise SystemExit(1)
