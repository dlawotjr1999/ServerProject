#!/usr/bin/env python3
import argparse
import socket
import struct
import sys
import threading
import time

# === Packet types (common.h 기준 가정) ===
PKT_CHAT = 1
PKT_JOIN_ROOM = 2
PKT_LEAVE_ROOM = 3

MAX_PACKET_SIZE = 1024  # 서버와 맞추기 (payload 최대)
MAX_LEN_FIELD = MAX_PACKET_SIZE + 2  # type(2)+payload

def pack_packet(pkt_type: int, payload: bytes) -> bytes:
    if payload is None:
        payload = b""
    if len(payload) > MAX_PACKET_SIZE:
        payload = payload[:MAX_PACKET_SIZE]
    length = 2 + len(payload)  # type(2) + payload
    return struct.pack("!HH", length, pkt_type) + payload

def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return b""
        buf += chunk
    return buf

class ChatClient:
    def __init__(self, host: str, port: int, local_echo: bool):
        self.host = host
        self.port = port
        self.local_echo = local_echo
        self.sock = None
        self.stop = threading.Event()
        self.rx_thread = None

    def connect(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((self.host, self.port))
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def send_pkt(self, pkt_type: int, payload: bytes = b""):
        if not self.sock:
            return
        data = pack_packet(pkt_type, payload)
        self.sock.sendall(data)

    def rx_loop(self):
        """
        서버가 보내는 스트림을 length 기반으로 파싱하여 출력
        """
        try:
            buf = b""
            while not self.stop.is_set():
                data = self.sock.recv(4096)
                if not data:
                    print("[INFO] disconnected by server")
                    self.stop.set()
                    break
                buf += data

                # 최소 4바이트(길이2 + 타입2)
                while len(buf) >= 4:
                    length, pkt_type = struct.unpack("!HH", buf[:4])

                    # 서버와 동일한 검증 범위
                    if length < 2 or length > MAX_LEN_FIELD:
                        print(f"[RX] protocol violation (length={length})")
                        self.stop.set()
                        return

                    total = 2 + length  # length필드(2) + (type+payload)
                    if len(buf) < total:
                        break

                    payload = buf[4:total]
                    buf = buf[total:]

                    # 출력
                    if pkt_type == PKT_CHAT:
                        # 서버 broadcast는 보통 텍스트(+개행)로 오므로 그대로 출력
                        try:
                            text = payload.decode(errors="replace")
                        except Exception:
                            text = repr(payload)
                        print(f"[CHAT] {text}", end="" if text.endswith("\n") else "\n")
                    else:
                        print(f"[PKT] type={pkt_type} payload_len={len(payload)} payload={payload!r}")
        except Exception as e:
            if not self.stop.is_set():
                print(f"[RX] error: {e}")
                self.stop.set()

    def start_rx(self):
        self.rx_thread = threading.Thread(target=self.rx_loop, daemon=True)
        self.rx_thread.start()

    def close(self):
        self.stop.set()
        try:
            if self.sock:
                self.sock.shutdown(socket.SHUT_RDWR)
        except Exception:
            pass
        try:
            if self.sock:
                self.sock.close()
        except Exception:
            pass
        self.sock = None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3800)
    ap.add_argument("--local-echo", action="store_true", help="내가 보낸 채팅도 로컬에 출력")
    args = ap.parse_args()

    c = ChatClient(args.host, args.port, args.local_echo)
    c.connect()
    c.start_rx()

    print("[INFO] connected.")
    print("Commands: /join  /leave  /quit")
    print("Type message to send chat.\n")

    try:
        while not c.stop.is_set():
            line = sys.stdin.readline()
            if not line:  # EOF
                break
            line = line.rstrip("\n")

            if line == "/quit":
                break
            elif line == "/join":
                c.send_pkt(PKT_JOIN_ROOM)
                print("[INFO] sent JOIN")
            elif line == "/leave":
                c.send_pkt(PKT_LEAVE_ROOM)
                print("[INFO] sent LEAVE")
            else:
                payload = line.encode()
                c.send_pkt(PKT_CHAT, payload)
                if args.local_echo:
                    print(f"[ME] {line}")
    except KeyboardInterrupt:
        pass
    finally:
        c.close()
        time.sleep(0.1)

if __name__ == "__main__":
    main()
