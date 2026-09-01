#!/usr/bin/env python3
"""
동시 접속/메시지 부하 테스트 도구.

client.py와 동일한 프레이밍 프로토콜을 쓰지만, 사람이 타이핑하는 대신 여러 클라이언트를
스레드로 동시에 띄워 JOIN -> CHAT(N개) -> LEAVE를 반복하고, 각 메시지의 송신 시각을 payload에
실어 보내 수신 측에서 왕복(end-to-end broadcast) 레이턴시를 계산한다.

같은 파이썬 프로세스 안에서 모든 클라이언트가 돌기 때문에 time.time() 하나로 모든 스레드의
시계가 자동으로 동기화되어 있다 - 별도 프로세스/머신 간 시계 동기화 문제를 걱정할 필요가 없다.

서버 아키텍처상 방 하나의 로컬 브로드캐스트조차 "직접 하지 않고 항상 Redis PUBLISH/SUBSCRIBE를
거친다"(REDESIGN.md §9) - 이 스크립트가 측정하는 레이턴시는 바로 그 왕복(자기 pod 안에서도 Redis를
거치는) 비용을 그대로 보여준다.

사용 예:
    python3 loadtest.py --host 127.0.0.1 --port 3800 --clients 100 --messages 20
"""
import argparse
import socket
import struct
import threading
import time

PKT_CHAT = 1
PKT_JOIN_ROOM = 2
PKT_LEAVE_ROOM = 3

MAX_PACKET_SIZE = 1024


def pack_packet(pkt_type: int, payload: bytes) -> bytes:
    if payload is None:
        payload = b""
    if len(payload) > MAX_PACKET_SIZE:
        payload = payload[:MAX_PACKET_SIZE]
    length = 2 + len(payload)
    return struct.pack("!HH", length, pkt_type) + payload


class LoadClient:
    def __init__(self, client_id: int, host: str, port: int, n_messages: int, send_interval: float):
        self.client_id = client_id
        self.host = host
        self.port = port
        self.n_messages = n_messages
        self.send_interval = send_interval

        self.sock = None
        self.stop = threading.Event()
        self.rx_thread = None

        self.connected = False
        self.joined_ok = False
        self.sent = 0
        self.received = 0
        self.latencies_ms = []  # 이 클라이언트가 "수신"한 다른 클라이언트들의 메시지 왕복 레이턴시

    def _rx_loop(self):
        buf = b""
        try:
            while not self.stop.is_set():
                data = self.sock.recv(4096)
                if not data:
                    break
                buf += data
                while len(buf) >= 4:
                    length, pkt_type = struct.unpack("!HH", buf[:4])
                    if length < 2 or length > MAX_PACKET_SIZE + 2:
                        return
                    total = 2 + length
                    if len(buf) < total:
                        break
                    payload = buf[4:total]
                    buf = buf[total:]

                    if pkt_type == PKT_CHAT:
                        self._on_chat(payload)
        except OSError:
            pass

    def _on_chat(self, payload: bytes):
        # payload 형식: "<client_id>|<seq>|<send_ts>\n" (send()가 붙인 개행 포함될 수 있음)
        try:
            text = payload.decode(errors="ignore").strip()
            parts = text.split("|")
            if len(parts) != 3:
                return
            sender_id = int(parts[0])
            send_ts = float(parts[2])
        except (ValueError, IndexError):
            return

        if sender_id == self.client_id:
            return  # 자기 자신이 보낸 메시지는 서버가 이미 제외하므로 여기 오면 안 되지만 방어적으로 스킵

        now = time.time()
        self.latencies_ms.append((now - send_ts) * 1000.0)
        self.received += 1

    def run(self):
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(10)
            self.sock.connect((self.host, self.port))
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.sock.settimeout(None)
            self.connected = True
        except OSError:
            return

        self.rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
        self.rx_thread.start()

        try:
            self.sock.sendall(pack_packet(PKT_JOIN_ROOM, b""))
            self.joined_ok = True
            time.sleep(0.2)  # 매치메이킹 + 구독이 자리잡을 시간

            for seq in range(self.n_messages):
                payload = f"{self.client_id}|{seq}|{time.time():.6f}".encode()
                self.sock.sendall(pack_packet(PKT_CHAT, payload))
                self.sent += 1
                time.sleep(self.send_interval)

            time.sleep(0.5)  # 다른 클라이언트들의 마지막 메시지를 마저 받을 유예 시간
            self.sock.sendall(pack_packet(PKT_LEAVE_ROOM, b""))
        except OSError:
            pass
        finally:
            self.stop.set()
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.sock.close()


def percentile(sorted_vals, p):
    if not sorted_vals:
        return float("nan")
    idx = min(len(sorted_vals) - 1, int(len(sorted_vals) * p / 100.0))
    return sorted_vals[idx]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3800)
    ap.add_argument("--clients", type=int, default=100, help="동시 연결 수")
    ap.add_argument("--messages", type=int, default=20, help="클라이언트당 전송 메시지 수")
    ap.add_argument("--send-interval", type=float, default=0.1, help="같은 클라이언트의 메시지 간 간격(초)")
    args = ap.parse_args()

    clients = [
        LoadClient(i, args.host, args.port, args.messages, args.send_interval)
        for i in range(args.clients)
    ]
    threads = [threading.Thread(target=c.run) for c in clients]

    t_start = time.time()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    t_end = time.time()

    connected = sum(1 for c in clients if c.connected)
    joined = sum(1 for c in clients if c.joined_ok)
    total_sent = sum(c.sent for c in clients)
    total_received = sum(c.received for c in clients)
    all_latencies = sorted(l for c in clients for l in c.latencies_ms)

    duration = t_end - t_start

    print("=" * 60)
    print("부하 테스트 결과")
    print("=" * 60)
    print(f"대상: {args.host}:{args.port}")
    print(f"요청 동시 연결 수: {args.clients}, 실제 연결 성공: {connected}, JOIN 성공: {joined}")
    print(f"클라이언트당 메시지 수: {args.messages}")
    print(f"총 소요 시간: {duration:.2f}s")
    print(f"총 송신 메시지: {total_sent} ({total_sent / duration:.1f} msg/s)")
    print(f"총 수신(브로드캐스트) 메시지: {total_received} ({total_received / duration:.1f} msg/s)")
    if all_latencies:
        print(f"레이턴시 샘플 수: {len(all_latencies)}")
        print(f"  p50: {percentile(all_latencies, 50):.2f} ms")
        print(f"  p95: {percentile(all_latencies, 95):.2f} ms")
        print(f"  p99: {percentile(all_latencies, 99):.2f} ms")
        print(f"  max: {max(all_latencies):.2f} ms")
    else:
        print("레이턴시 샘플 없음 (방마다 인원이 1명뿐이었거나 수신 실패)")
    print("=" * 60)


if __name__ == "__main__":
    main()
