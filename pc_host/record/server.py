"""TCP 50002 recording server: validates chunks and atomically finalizes WAVs."""
from __future__ import annotations
import json
import socket
import sqlite3
import threading
import time
from pathlib import Path
from ..protocol import CHUNK, crc16_ccitt, wav_header

class RecordingServer:
    def __init__(self, root: str | Path = "recordings", host: str = "", port: int = 50002) -> None:
        self.root = Path(root); self.root.mkdir(parents=True, exist_ok=True); self.host, self.port = host, port
        self.db = sqlite3.connect(self.root / "index.sqlite", check_same_thread=False)
        self.db.execute("CREATE TABLE IF NOT EXISTS recordings(session_id TEXT PRIMARY KEY, path TEXT, bytes INTEGER, created REAL)"); self.db.commit()
        self._stop = threading.Event(); self._sock: socket.socket | None = None; self._thread: threading.Thread | None = None

    def start(self) -> None:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM); self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); self._sock.bind((self.host, self.port)); self._sock.listen(); self._sock.settimeout(.2)
        self._thread = threading.Thread(target=self._accept, name="record-server", daemon=True); self._thread.start()

    def close(self) -> None:
        self._stop.set()
        if self._thread: self._thread.join(1)
        if self._sock: self._sock.close()
        self.db.close()

    def list(self) -> list[tuple[str, str, int, float]]:
        return list(self.db.execute("SELECT session_id,path,bytes,created FROM recordings ORDER BY created DESC"))

    def delete(self, session_id: str) -> None:
        row = self.db.execute("SELECT path FROM recordings WHERE session_id=?", (session_id,)).fetchone()
        if row: Path(row[0]).unlink(missing_ok=True)
        self.db.execute("DELETE FROM recordings WHERE session_id=?", (session_id,)); self.db.commit()

    def _accept(self) -> None:
        assert self._sock
        while not self._stop.is_set():
            try: conn, _ = self._sock.accept()
            except socket.timeout: continue
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    @staticmethod
    def _read_exact(conn: socket.socket, size: int) -> bytes:
        data = bytearray()
        while len(data) < size:
            piece = conn.recv(size - len(data))
            if not piece: raise ConnectionError("connection closed")
            data.extend(piece)
        return bytes(data)

    def _handle(self, conn: socket.socket) -> None:
        session = ""; path: Path | None = None; total_bytes: int | None = None
        with conn:
            try:
                line = bytearray()
                while not line.endswith(b"\n"):
                    line.extend(self._read_exact(conn, 1))
                begin = json.loads(line)
                if begin.get("type") != "REC_BEGIN": return
                session = str(begin["session_id"]); path = self.root / f"{session}.pcm.part"; base = int(begin.get("base_offset", 0))
                print(f"[rec] BEGIN session={session} base={base}", flush=True)
                conn.sendall((json.dumps({"type":"REC_BEGIN_ACK","session_id":session,"accepted":True}) + "\n").encode())
                with path.open("r+b" if path.exists() else "w+b") as file:
                    while True:
                        prefix = self._read_exact(conn, 1)
                        if prefix == b"{":
                            line = prefix + self._read_until_newline(conn)
                            end = json.loads(line)
                            if end.get("type") == "REC_END":
                                total_bytes = max(file.tell(), int(end.get("total_bytes", 0)))
                                print(f"[rec] END session={session} total={total_bytes} file_tell={file.tell()}", flush=True)
                                break
                            continue
                        raw_len = prefix + self._read_exact(conn, 3); chunk_len = int.from_bytes(raw_len, "little")
                        if chunk_len < 6 or chunk_len > 4102: raise ValueError("bad chunk length")
                        offset, crc = CHUNK.unpack(raw_len + self._read_exact(conn, 6))[1:]
                        payload = self._read_exact(conn, chunk_len - 6)
                        if crc16_ccitt(payload) != crc: raise ValueError("bad CRC")
                        file.seek(base + offset); file.write(payload); file.flush()
                if total_bytes is not None:
                    self._finish(session, path, total_bytes)
                    try: conn.sendall(b'{"type":"REC_END_ACK","saved":true}\n')
                    except OSError: pass
            except (ConnectionError, OSError, ValueError, json.JSONDecodeError) as e:
                print(f"[rec] closed: {type(e).__name__}: {e}", flush=True)
            finally:
                # connection dropped or ended: always finalize whatever .part we got
                if path and path.exists() and path.stat().st_size > 0:
                    sz = path.stat().st_size
                    if sz % 2: sz -= 1  # keep even
                    if path.with_suffix("").with_suffix(".wav") not in self.root.glob("*.wav") or total_bytes is not None:
                        try:
                            self._finish(session, path, total_bytes or sz)
                        except OSError as e:
                            print(f"[rec] finalize failed: {e}", flush=True)

    @staticmethod
    def _read_until_newline(conn: socket.socket) -> bytes:
        data = bytearray()
        while not data.endswith(b"\n"): data.extend(RecordingServer._read_exact(conn, 1))
        return bytes(data)

    def _finish(self, session: str, part: Path, size: int) -> None:
        wav = self.root / f"{session}.wav"
        with part.open("rb") as source, wav.open("wb") as target:
            target.write(wav_header(size)); target.write(source.read(size))
        part.unlink(missing_ok=True)
        self.db.execute("INSERT OR REPLACE INTO recordings VALUES(?,?,?,?)", (session, str(wav), size, time.time())); self.db.commit()
        print(f"[rec] saved {wav.name} ({size} bytes)", flush=True)
