#!/usr/bin/env python3
import argparse
import asyncio
import base64
import contextlib
import json
import logging
import signal
import time
from dataclasses import dataclass
from typing import Any, Dict, Optional

from aiortc import RTCConfiguration, RTCIceCandidate, RTCIceServer, RTCPeerConnection, RTCSessionDescription
from aiortc.contrib.media import MediaBlackhole, MediaRecorder
from aiortc.sdp import candidate_from_sdp, candidate_to_sdp


logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
LOG = logging.getLogger("python_receiver")


@dataclass
class MetricsState:
    last_packets_received: int = 0
    last_packets_lost: int = 0
    last_bytes_received: int = 0
    last_ts: float = 0.0


class TcpJsonLineSignaling:
    def __init__(self, host: str, port: int) -> None:
        self._host = host
        self._port = port
        self._server: Optional[asyncio.AbstractServer] = None
        self._reader: Optional[asyncio.StreamReader] = None
        self._writer: Optional[asyncio.StreamWriter] = None
        self._connected = asyncio.Event()

    async def start(self) -> None:
        self._server = await asyncio.start_server(self._on_client, self._host, self._port)
        LOG.info("TCP signaling listening on %s:%d", self._host, self._port)

    async def wait_connected(self) -> None:
        await self._connected.wait()

    async def close(self) -> None:
        if self._writer is not None:
            self._writer.close()
            await self._writer.wait_closed()
            self._writer = None
            self._reader = None
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
            self._server = None

    async def recv(self) -> Optional[Dict[str, Any]]:
        if self._reader is None:
            return None
        line = await self._reader.readline()
        if not line:
            return {"type": "__eof__"}
        text = line.decode("utf-8", errors="ignore").strip()
        if not text:
            return None
        try:
            return json.loads(text)
        except json.JSONDecodeError:
            LOG.warning("ignore invalid signaling json: %s", text)
            return None

    async def send(self, payload: Dict[str, Any]) -> None:
        if self._writer is None:
            return
        data = json.dumps(payload, separators=(",", ":")) + "\n"
        self._writer.write(data.encode("utf-8"))
        await self._writer.drain()

    async def _on_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        if self._writer is not None:
            writer.close()
            await writer.wait_closed()
            LOG.warning("reject extra signaling client")
            return

        self._reader = reader
        self._writer = writer
        self._connected.set()
        peer = writer.get_extra_info("peername")
        LOG.info("TCP signaling connected: %s", peer)


def decode_sdp_base64(encoded: str) -> str:
    return base64.b64decode(encoded.encode("utf-8")).decode("utf-8", errors="ignore")


def encode_sdp_base64(text: str) -> str:
    return base64.b64encode(text.encode("utf-8")).decode("utf-8")


def to_ms(value: Optional[float]) -> float:
    if value is None:
        return 0.0
    if value < 10.0:
        return value * 1000.0
    return value


async def metrics_loop(
    signaling: TcpJsonLineSignaling,
    pc: RTCPeerConnection,
    state: MetricsState,
    interval: float,
) -> None:
    while True:
        await asyncio.sleep(interval)
        try:
            stats = await pc.getStats()
        except Exception as exc:
            LOG.warning("getStats failed: %s", exc)
            continue

        packets_received = 0
        packets_lost = 0
        bytes_received = 0
        jitter_ms = 0.0
        rtt_ms = 0.0

        for report in stats.values():
            report_type = getattr(report, "type", "")
            if report_type == "inbound-rtp":
                packets_received = max(packets_received, int(getattr(report, "packetsReceived", 0) or 0))
                packets_lost = max(packets_lost, int(getattr(report, "packetsLost", 0) or 0))
                bytes_received = max(bytes_received, int(getattr(report, "bytesReceived", 0) or 0))
                jitter_ms = max(jitter_ms, to_ms(getattr(report, "jitter", 0.0)))

            if report_type in {"remote-inbound-rtp", "candidate-pair"}:
                rtt_ms = max(
                    rtt_ms,
                    to_ms(
                        getattr(report, "roundTripTime", None)
                        or getattr(report, "currentRoundTripTime", None)
                    ),
                )

        now = time.monotonic()
        previous_total = state.last_packets_received + state.last_packets_lost
        current_total = packets_received + packets_lost
        delta_total = max(0, current_total - previous_total)
        delta_lost = max(0, packets_lost - state.last_packets_lost)
        packet_loss_ratio = (float(delta_lost) / float(delta_total)) if delta_total > 0 else 0.0

        estimated_kbps = 0
        if state.last_ts > 0 and now > state.last_ts and bytes_received >= state.last_bytes_received:
            delta_bytes = bytes_received - state.last_bytes_received
            delta_ms = (now - state.last_ts) * 1000.0
            if delta_ms > 0:
                estimated_kbps = int((delta_bytes * 8.0) / delta_ms)

        state.last_packets_received = packets_received
        state.last_packets_lost = packets_lost
        state.last_bytes_received = bytes_received
        state.last_ts = now

        await signaling.send(
            {
                "type": "metrics",
                "packetLossRatio": packet_loss_ratio,
                "rttMs": rtt_ms,
                "jitterMs": jitter_ms,
                "estimatedKbps": estimated_kbps,
            }
        )


async def run(args: argparse.Namespace) -> None:
    ice_servers = []
    if args.stun_server:
        ice_servers.append(RTCIceServer(urls=args.stun_server))
    if args.turn_server:
        ice_servers.append(
            RTCIceServer(
                urls=args.turn_server,
                username=args.turn_username or None,
                credential=args.turn_password or None,
            )
        )
    pc = RTCPeerConnection(configuration=RTCConfiguration(iceServers=ice_servers))
    signaling = TcpJsonLineSignaling(args.host, args.port)
    metrics_state = MetricsState()
    metrics_task: Optional[asyncio.Task] = None

    sink: Any
    if not args.record:
        sink = MediaBlackhole()
    else:
        sink = MediaRecorder(args.record)

    @pc.on("icecandidate")
    def on_icecandidate(candidate: Optional[RTCIceCandidate]) -> None:
        if candidate is None:
            return
        asyncio.create_task(
            signaling.send(
                {
                    "type": "ice",
                    "mlineIndex": int(candidate.sdpMLineIndex or 0),
                    "candidate": "candidate:" + candidate_to_sdp(candidate),
                }
            )
        )

    @pc.on("track")
    def on_track(track) -> None:
        LOG.info("remote track received: kind=%s id=%s", track.kind, track.id)
        sink.addTrack(track)

    await signaling.start()
    await signaling.wait_connected()

    started_sink = False

    try:
        while True:
            message = await signaling.recv()
            if message is None:
                await asyncio.sleep(0.02)
                continue

            msg_type = message.get("type", "")
            if msg_type == "__eof__":
                LOG.warning("signaling connection closed")
                break
            if msg_type == "sdp" and message.get("sdpType") == "offer":
                offer_sdp = decode_sdp_base64(message.get("sdpBase64", ""))
                offer = RTCSessionDescription(sdp=offer_sdp, type="offer")
                await pc.setRemoteDescription(offer)

                answer = await pc.createAnswer()
                await pc.setLocalDescription(answer)

                await signaling.send(
                    {
                        "type": "sdp",
                        "sdpType": "answer",
                        "sdpBase64": encode_sdp_base64(pc.localDescription.sdp),
                    }
                )
                LOG.info("sent answer")

                if not started_sink:
                    await sink.start()
                    started_sink = True

                if metrics_task is None:
                    metrics_task = asyncio.create_task(metrics_loop(signaling, pc, metrics_state, args.metrics_interval))

            elif msg_type == "ice":
                candidate_line = message.get("candidate", "")
                mline_index = int(message.get("mlineIndex", 0))
                if not candidate_line:
                    continue
                try:
                    raw_candidate = candidate_line.split(":", 1)[1] if ":" in candidate_line else candidate_line
                    candidate = candidate_from_sdp(raw_candidate)
                    candidate.sdpMid = "0"
                    candidate.sdpMLineIndex = mline_index
                    await pc.addIceCandidate(candidate)
                except Exception as exc:
                    LOG.warning("failed to add ICE candidate: %s", exc)

    finally:
        if metrics_task is not None:
            metrics_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await metrics_task
        await sink.stop()
        await pc.close()
        await signaling.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Python WebRTC receiver compatible with weaknet sender signaling.")
    parser.add_argument("--host", default="127.0.0.1", help="TCP signaling listen host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=9000, help="TCP signaling listen port (default: 9000)")
    parser.add_argument(
        "--record",
        default="",
        help="Optional output file path (e.g. out.mp4). Empty means discard media.",
    )
    parser.add_argument(
        "--stun-server",
        default="stun:stun.l.google.com:19302",
        help="STUN server URL, empty to disable",
    )
    parser.add_argument("--turn-server", default="", help="TURN server URL, optional")
    parser.add_argument("--turn-username", default="", help="TURN username, optional")
    parser.add_argument("--turn-password", default="", help="TURN password, optional")
    parser.add_argument(
        "--metrics-interval",
        type=float,
        default=1.0,
        help="Metrics reporting interval seconds (default: 1.0)",
    )
    return parser.parse_args()


if __name__ == "__main__":
    arguments = parse_args()
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    stop_event = asyncio.Event()

    def _stop() -> None:
        if not stop_event.is_set():
            stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            pass

    async def _main() -> None:
        runner = asyncio.create_task(run(arguments))
        stopper = asyncio.create_task(stop_event.wait())
        done, pending = await asyncio.wait({runner, stopper}, return_when=asyncio.FIRST_COMPLETED)
        for task in pending:
            task.cancel()
        if stopper in done and not runner.done():
            runner.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await runner
        elif runner in done:
            await runner

    loop.run_until_complete(_main())
