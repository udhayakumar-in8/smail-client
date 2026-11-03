# Minimal SMTP sink for tests: accepts one session at a time, prints envelope + size.
import socket, sys
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(5)
while True:
    c, _ = s.accept(); f = c.makefile("rb")
    def send(x): c.sendall((x + "\r\n").encode())
    send("220 sink ready")
    data = False
    for line in f:
        l = line.decode(errors="replace").rstrip("\r\n")
        if data:
            if l == ".":
                data = False; send("250 queued"); print("DATA done", flush=True)
            continue
        cmd = l.upper()
        if cmd.startswith("EHLO"): send("250-sink\r\n250 SIZE 1000000")
        elif cmd.startswith("HELO"): send("250 sink")
        elif cmd.startswith(("MAIL", "RCPT")): print(l, flush=True); send("250 ok")
        elif cmd.startswith("DATA"): data = True; send("354 go")
        elif cmd.startswith("QUIT"): send("221 bye"); break
        else: send("250 ok")
    c.close()
