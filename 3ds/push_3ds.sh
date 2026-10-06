#!/bin/sh
# Push build3ds/ac_3ds.3dsx to the 3DS Homebrew Launcher netloader, then read the game's
# live log over Wi-Fi for -t seconds.
# Usage (repo root, Git Bash): sh 3ds/push_3ds.sh [-t seconds] [game args...]
#   e.g. sh 3ds/push_3ds.sh -t 90 gputest
# The 3DS address comes from $N3DS_IP, else from a UDP broadcast (the same probe 3dslink uses).
# The game runs a log server on TCP port 17492 when netloaded (3ds/src/n3ds_sys.c). This PC
# connects to it, the same direction as the upload, so a PC firewall or VPN does not block it.
# Output: build3ds/hw_live_log.txt (also printed). Run it in the background for long runs.
set -e
cd "$(dirname "$0")/.."
secs=60
if [ "$1" = "-t" ]; then secs=$2; shift 2; fi

ip=${N3DS_IP:-$(py -3 - <<'EOF'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
s.bind(("", 17491))
s.settimeout(1.0)
for _ in range(8):
    s.sendto(b"3dsboot", ("255.255.255.255", 17491))
    try:
        data, addr = s.recvfrom(64)
        if data.startswith(b"boot3ds"):
            print(addr[0]); break
    except socket.timeout:
        pass
EOF
)}
if [ -z "$ip" ]; then echo "no 3DS netloader found (press Y in the Homebrew Launcher)"; exit 1; fi
echo "3DS at $ip"

MSYS_NO_PATHCONV=1 timeout 120 docker run --rm -v "$(pwd)/build3ds:/out:ro" devkitpro/devkitarm:latest \
    stdbuf -oL /opt/devkitpro/tools/bin/3dslink -a "$ip" /out/ac_3ds.3dsx -- --verbose "$@"

echo "reading the game's log from $ip:17492 for $secs s"
py -3 -u - "$ip" "$secs" <<'EOF'
import socket, sys, time
ip, secs = sys.argv[1], float(sys.argv[2])
end = time.time() + secs
out = open("build3ds/hw_live_log.txt", "wb")
sock = None
while sock is None and time.time() < end:
    try:
        sock = socket.create_connection((ip, 17492), timeout=2)
    except OSError:
        time.sleep(1)  # the game opens the server a few seconds after the upload
if sock is None:
    print("no connection to the game's log server (see the 'live log:' line on the bottom screen)")
    sys.exit(1)
print("connected")
sock.settimeout(1)
while time.time() < end:
    try:
        data = sock.recv(4096)
    except socket.timeout:
        continue
    except OSError as e:
        print("connection lost:", e)
        break
    if not data:
        print("game closed the connection")
        break
    out.write(data.replace(b"\r", b""))
    out.flush()
    sys.stdout.write(data.decode("utf-8", "replace"))
EOF
echo "--- log saved to build3ds/hw_live_log.txt"
