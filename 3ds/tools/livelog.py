"""livelog.py - read the game's live log from a 3DS (or Azahar) over the network (TCP 17492).

Usage: py -3 -u 3ds/tools/livelog.py <3ds-ip> [seconds] [out-file]
On connect the game sends the last 32 KB of its log, then each new line. Each line starts with
"#<seq> ". After a reconnect, lines already received are skipped by that number, and a gap in
the numbers is reported, so a lost line is never silent. Writes raw bytes, so Japanese debug
text cannot break the Windows console.
"""
import re
import socket
import sys
import time

SEQ = re.compile(rb"^#(\d+) ")


def main():
    ip = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 60
    path = sys.argv[3] if len(sys.argv) > 3 else "build3ds/hw_live_log.txt"
    end = time.time() + secs
    out = open(path, "wb")
    stdout = sys.stdout.buffer
    last_seq = -1
    gaps = 0
    connects = 0
    buf = b""

    def emit(line):
        out.write(line)
        out.flush()
        stdout.write(line)
        stdout.flush()

    while time.time() < end:
        try:
            sock = socket.create_connection((ip, 17492), timeout=2)
        except OSError:
            time.sleep(1)  # the game opens the server a few seconds after it starts
            continue
        connects += 1
        print("[livelog] connected" if connects == 1 else "[livelog] reconnected", flush=True)
        sock.settimeout(1)
        buf = b""
        while time.time() < end:
            try:
                data = sock.recv(4096)
            except socket.timeout:
                continue
            except OSError as e:
                print("[livelog] connection lost:", e, flush=True)
                break
            if not data:
                print("[livelog] game closed the connection", flush=True)
                break
            buf += data.replace(b"\r", b"")
            *lines, buf = buf.split(b"\n")
            for line in lines:
                m = SEQ.match(line)
                if m:
                    seq = int(m.group(1))
                    if seq <= last_seq:
                        continue  # already received before the reconnect
                    if last_seq >= 0 and seq != last_seq + 1:
                        gaps += 1
                        emit(b"[livelog] GAP: lines #%d-#%d missing\n" % (last_seq + 1, seq - 1))
                    last_seq = seq
                emit(line + b"\n")
        sock.close()
    if buf:
        emit(buf + b"\n")
    if not connects:
        print("[livelog] no connection to the game's log server (see the 'live log:' line on the bottom screen)")
        sys.exit(1)
    print("[livelog] done: last line #%d, %d gaps, %d connects" % (last_seq, gaps, connects), flush=True)


if __name__ == "__main__":
    main()
