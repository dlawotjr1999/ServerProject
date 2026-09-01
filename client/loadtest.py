#!/usr/bin/env python3
"""
동시 접속/메시지 부하 테스트 도구 (asyncio 기반).

client.py와 동일한 프레이밍 프로토콜을 쓰지만, 사람이 타이핑하는 대신 여러 클라이언트를
asyncio 태스크로 동시에 띄워 JOIN -> CHAT(N개) -> LEAVE를 반복하고, 각 메시지의 송신 시각을
payload에 실어 보내 수신 측에서 왕복(end-to-end broadcast) 레이턴시를 계산한다.

같은 파이썬 프로세스 안에서 모든 클라이언트가 돌기 때문에 time.time() 하나로 모든 태스크의
시계가 자동으로 동기화되어 있다 - 별도 프로세스/머신 간 시계 동기화 문제를 걱정할 필요가 없다.

asyncio(단일 스레드 이벤트 루프)로 짠 이유: 초기 버전은 threading으로(클라이언트 수만큼 OS
스레드) 구현했는데, 동시 클라이언트가 늘어날수록 파이썬 GIL 경합이 서버보다 먼저 병목이 되어
p95/p99 tail latency가 서버 아키텍처와 무관하게 비슷한 값으로 수렴하는 걸 실측으로 확인했다
(docs/PORTFOLIO.md "성능" 절 참고). asyncio는 클라이언트가 각자 OS 스레드를 쓰지 않고 하나의
이벤트 루프 안에서 협조적으로(cooperatively) 스케줄되므로, 이 병목을 없애고 더 높은 동시성에서
더 깨끗한 레이턴시 신호를 얻을 수 있다.

서버 아키텍처상 방 하나의 로컬 브로드캐스트조차 "직접 하지 않고 항상 Redis PUBLISH/SUBSCRIBE를
거친다"(REDESIGN.md §9) - 이 스크립트가 측정하는 레이턴시는 바로 그 왕복(자기 pod 안에서도 Redis를
거치는) 비용을 그대로 보여준다.

사용 예:
    python3 loadtest.py --host 127.0.0.1 --port 3800 --clients 500 --messages 20
"""
import argparse
import asyncio
import struct
import time

PKT_CHAT = 1
PKT_JOIN_ROOM = 2
PKT_LEAVE_ROOM = 3

MAX_PACKET_SIZE = 1024


def pack_packet(pkt_type: int, payload: bytes = b"") -> bytes:
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

        self.connected = False
        self.joined_ok = False
        self.sent = 0
        self.received = 0
        self.latencies_ms = []  # 이 클라이언트가 "수신"한 다른 클라이언트들의 메시지 왕복 레이턴시

    def _on_chat(self, payload: bytes):
        # payload 형식: "<client_id>|<seq>|<send_ts>"
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

    async def _rx_loop(self, reader: asyncio.StreamReader):
        buf = b""
        try:
            while True:
                data = await reader.read(4096)
                if not data:
                    return
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
        except (ConnectionResetError, OSError):
            pass

    async def run(self):
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port), timeout=10
            )
        except (OSError, asyncio.TimeoutError):
            return

        self.connected = True
        sock = writer.get_extra_info("socket")
        if sock is not None:
            try:
                import socket as _socket
                sock.setsockopt(_socket.IPPROTO_TCP, _socket.TCP_NODELAY, 1)
            except OSError:
                pass

        rx_task = asyncio.ensure_future(self._rx_loop(reader))

        try:
            writer.write(pack_packet(PKT_JOIN_ROOM))
            await writer.drain()
            self.joined_ok = True
            await asyncio.sleep(0.2)  # 매치메이킹 + 구독이 자리잡을 시간

            for seq in range(self.n_messages):
                payload = f"{self.client_id}|{seq}|{time.time():.6f}".encode()
                writer.write(pack_packet(PKT_CHAT, payload))
                await writer.drain()
                self.sent += 1
                await asyncio.sleep(self.send_interval)

            await asyncio.sleep(0.5)  # 다른 클라이언트들의 마지막 메시지를 마저 받을 유예 시간
            writer.write(pack_packet(PKT_LEAVE_ROOM))
            await writer.drain()
        except (OSError, ConnectionResetError):
            pass
        finally:
            rx_task.cancel()
            try:
                writer.close()
            except OSError:
                pass


def percentile(sorted_vals, p):
    if not sorted_vals:
        return float("nan")
    idx = min(len(sorted_vals) - 1, int(len(sorted_vals) * p / 100.0))
    return sorted_vals[idx]


async def run_all(args):
    clients = [
        LoadClient(i, args.host, args.port, args.messages, args.send_interval)
        for i in range(args.clients)
    ]

    t_start = time.time()
    await asyncio.gather(*(c.run() for c in clients))
    t_end = time.time()

    connected = sum(1 for c in clients if c.connected)
    joined = sum(1 for c in clients if c.joined_ok)
    total_sent = sum(c.sent for c in clients)
    total_received = sum(c.received for c in clients)
    all_latencies = sorted(l for c in clients for l in c.latencies_ms)

    duration = t_end - t_start

    print("=" * 60)
    print("부하 테스트 결과 (asyncio)")
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3800)
    ap.add_argument("--clients", type=int, default=100, help="동시 연결 수")
    ap.add_argument("--messages", type=int, default=20, help="클라이언트당 전송 메시지 수")
    ap.add_argument("--send-interval", type=float, default=0.1, help="같은 클라이언트의 메시지 간 간격(초)")
    args = ap.parse_args()

    asyncio.run(run_all(args))


if __name__ == "__main__":
    main()
