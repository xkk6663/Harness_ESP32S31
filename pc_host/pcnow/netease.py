"""网易云非官方 Web API 封装：搜歌 → 歌词 / 封面。约 1-2 次/歌，频率友好。

接口实测（2026-09-23）：
  搜索:  https://music.163.com/api/search/get?s=<kw>&type=1&limit=5
         ⚠ 结果里 album.picUrl 已改为空串（拼 ?param=240y240 是无效 URL）
  封面:  https://music.163.com/api/song/detail?ids=[<id>] 仍返回 album.picUrl
         （="//p2.music.126.net/..." 协议相对，需补 https: 前缀）
  歌词:  https://music.163.com/api/song/lyric?os=pc&id=<id>&lv=-1&tv=-1
"""
from __future__ import annotations

import hashlib
import re
import time
from dataclasses import dataclass, field

import requests

_UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Mozilla/5.0",
       "Referer": "https://music.163.com"}
_TIMEOUT = 6.0

# 与 C# agent 相同的制作人员行过滤（作词/作曲/编曲…）
_CREDITS = re.compile(r"^(作词|作曲|编曲|制作|监制|混音|母带|录音|和声|吉他|贝斯|鼓|键盘|弦乐|配唱|填词|谱曲|OP|SP)")
_LRC_TS = re.compile(r"\[(\d+):(\d+(?:\.\d+)?)\]")


def track_hash(title: str, artist: str) -> str:
    """SHA1(title|artist) 前 8 hex —— 与设备端 / PC agent 的 hash 规则一致。"""
    return hashlib.sha1(f"{title}|{artist}".encode("utf-8")).hexdigest()[:8]


@dataclass
class TrackData:
    hash: str = ""
    title: str = ""
    artist: str = ""
    duration_ms: int = 0                            # 网易云曲库时长（SMTC 无时间轴时的唯一曲长源）
    lyrics: list = field(default_factory=list)     # [(t_ms: int, text: str)] 已排序
    cover: bytes = b""
    lyric_err: str = ""
    cover_err: str = ""


def _get(url: str, params: dict | None = None) -> dict:
    r = requests.get(url, params=params, headers=_UA, timeout=_TIMEOUT)
    r.raise_for_status()
    return r.json()


def search_song(title: str, artist: str) -> dict | None:
    """返回 {id, pic_url, name, artists}；按歌手名包含关系挑最优。

    pic_url 现从 song/detail 二次获取（搜索接口已不返回 picUrl，见文件头）。
    """
    kw = f"{title} {artist}".strip()
    try:
        data = _get("https://music.163.com/api/search/get", {"s": kw, "type": 1, "limit": 5})
    except Exception:
        return None
    songs = (data.get("result") or {}).get("songs") or []
    for s in songs:
        names = " ".join(a.get("name", "") for a in (s.get("artists") or []))
        if artist and artist.lower() in names.lower():
            return {"id": s.get("id"), "name": s.get("name", ""), "artists": names}
    if songs:
        s = songs[0]
        return {"id": s.get("id"), "name": s.get("name", ""),
                "artists": " ".join(a.get("name", "") for a in (s.get("artists") or []))}
    return None


def _song_detail(song_id: int) -> tuple:
    """song/detail 接口：返回 (pic_url, duration_ms)。
    ★ 曲长来自网易云曲库 —— 网易云 Win32 客户端不上报 SMTC 时间轴（实测 pos/end
      全为 None），这是副屏曲长/进度的唯一来源。picUrl 必须 ?param=240y240
      （CDN 缩放，原图超设备 96KB 拉取上限且解码器限 240x240）。"""
    try:
        data = _get("https://music.163.com/api/song/detail", {"ids": f"[{song_id}]"})
        songs = data.get("songs") or []
        if songs:
            s0 = songs[0]
            pic = (s0.get("album") or {}).get("picUrl") or ""
            if pic:
                pic += ("&" if "?" in pic else "?") + "param=240y240"
            dur = int(s0.get("duration") or 0)
            return pic, dur
    except Exception:
        pass
    return "", 0


def fetch_lyric(song_id: int) -> str:
    try:
        data = _get("https://music.163.com/api/song/lyric",
                    {"os": "pc", "id": song_id, "lv": -1, "tv": -1})
        return (data.get("lrc") or {}).get("lyric") or ""
    except Exception:
        return ""


def fetch_cover(pic_url: str) -> bytes:
    if not pic_url:
        return b""
    if pic_url.startswith("//"):             # 协议相对 URL（网易 CDN 常见）
        pic_url = "https:" + pic_url
    try:
        r = requests.get(pic_url, headers=_UA, timeout=_TIMEOUT)
        r.raise_for_status()
        return r.content
    except Exception:
        return b""


# 设备端约束（pcnow_service.c）：HTTP_BUF_MAX = 96KB 单次拉取上限，
# 解码器只接受 ≤240x240（COVER_W_MAX/H_MAX）。
COVER_MAX_PX = 240
COVER_MAX_BYTES = 64 * 1024


def _shrink_cover(raw: bytes) -> bytes:
    """把封面**归一化**成 ≤240x240 且 ≤64KB 的 JPEG。

    ★ 为什么必须在 PC 侧做（§28.53）：`?param=240y240` 是网易客户端约定，
    CDN 一旦没照办（改写/忽略/302 到原图），PC 拿到的就是 120~200KB+ 的原图，
    设备端会同时踩两个坑 ——
      ① 撞 96KB 拉取上限 ⇒ 字节被截断；
      ② 尺寸超过解码器 240x240 上限 ⇒ 就算不截断也会被拒。
    在 PC 上压成确定的小图，设备端就只当"哑终端"，不用为图片尺寸操心。
    Pillow 缺失时退回"原样但要够小"，绝不静默返回不可用的数据。
    """
    if not raw or raw[:2] != b"\xff\xd8":     # 非 JPEG（HTML 错误页/502 文本…）
        return b""
    try:
        from PIL import Image
    except ImportError:
        return raw if len(raw) <= COVER_MAX_BYTES else b""
    try:
        import io
        im = Image.open(io.BytesIO(raw))
        # 本来就在设备端能力范围内 ⇒ 原样透传：省一次重编码的 CPU，也不掉画质
        # （CDN 出的 240y240 通常只有 5~13KB，重编码反而会变大、变糊）。
        if (im.width <= COVER_MAX_PX and im.height <= COVER_MAX_PX
                and len(raw) <= COVER_MAX_BYTES):
            return raw
        im = im.convert("RGB")
        if im.width > COVER_MAX_PX or im.height > COVER_MAX_PX:
            resample = getattr(getattr(Image, "Resampling", Image), "LANCZOS", 1)
            im.thumbnail((COVER_MAX_PX, COVER_MAX_PX), resample)
        for quality in (88, 70):
            buf = io.BytesIO()
            im.save(buf, "JPEG", quality=quality, optimize=True)
            out = buf.getvalue()
            if len(out) <= COVER_MAX_BYTES:
                return out
        return b""
    except Exception:
        return raw if len(raw) <= COVER_MAX_BYTES else b""


def parse_lrc(lrc_text: str) -> list:
    """解析 LRC 为 [(t_ms, text)]；过滤制作人员行与空行，按时间排序。

    ★ t 是**毫秒整数**（ICD §7.3 契约：`{"lines":[{"t":<ms>,"text":".."}...]}`）。
      这里曾是本工程最隐蔽的一个 bug：Python 侧按**秒**（float）产出，而设备端
      `pcnow_service.c` 与 C# agent 一律按毫秒用，`(uint32_t)valuedouble` 会把
      4.2s 截成 4ms ⇒ 设备端二分查行**恒返回最后一行**，现象是"歌词一直停在
      最后一句、不随进度滚动"。改动单位时请同时核对：
        pc_host/pcnow/http_server.py（/lyrics.json 原样序列化）
        pc_host/pcnow/pusher.py（NOW_LRC 的 t、_line_for 的比较单位）
        pc_host/ui/pages/pcnow_page.py（本地预览按 ms 比较）
        tools/pc_now_agent/Program.cs（C# 版，本来就是 ms）
        docs/项目规划/通信接口规范_ICD.md §7.3
    """
    lines: list = []
    for raw in lrc_text.splitlines():
        raw = raw.strip()
        if not raw or _CREDITS.match(_LRC_TS.sub("", raw).strip()):
            continue
        for m in _LRC_TS.finditer(raw):
            text = _LRC_TS.sub("", raw).strip().strip("-—~ ")
            if text:
                sec = int(m.group(1)) * 60.0 + float(m.group(2))
                lines.append((int(round(sec * 1000)), text))
    lines.sort(key=lambda x: x[0])
    return lines


def resolve_track(title: str, artist: str) -> TrackData:
    """搜歌 + 拉歌词 + 拉封面（阻塞，调用方放工作线程）。"""
    td = TrackData(hash=track_hash(title, artist), title=title, artist=artist)
    song = search_song(title, artist)
    if not song:
        td.lyric_err = "未在网易云找到该曲"
        return td
    lrc = fetch_lyric(song["id"])
    if lrc:
        td.lyrics = parse_lrc(lrc)
    if not td.lyrics:
        td.lyric_err = "无歌词(纯音乐或接口未返回)" if not lrc else "歌词行全部为空/制作人员"
    pic, dur = _song_detail(song["id"])
    td.duration_ms = dur
    if pic:
        raw = fetch_cover(pic)
        td.cover = _shrink_cover(raw)
        if not td.cover:
            td.cover_err = (f"封面不可用（原始 {len(raw)}B：非 JPEG 或压缩后仍超 "
                            f"{COVER_MAX_BYTES}B）" if raw else "封面下载失败")
    else:
        td.cover_err = "搜索结果无封面链接"
    return td


if __name__ == "__main__":  # quick manual test
    _t0 = time.time()
    td = resolve_track("Love Song", "方大同")
    print(f"hash={td.hash} lyric_lines={len(td.lyrics)} cover={len(td.cover)}B "
          f"err={td.lyric_err or '-'} in {time.time()-_t0:.1f}s")
    for t, text in td.lyrics[:5]:
        print(f"  [{t / 1000:7.2f}s] {text}")
