// SPDX-License-Identifier: Apache-2.0
// pc_now_agent — PC 端推流代理（docs/项目规划/PC音乐副屏局域网推流实施方案.md V2 / ICD V1.3）
//
// 职责：
//  1) SMTC 采集：Windows Media Session（网易云/QQ音乐/foobar/浏览器通用）
//     曲名/歌手/专辑/时长/进度/播放态/封面
//  2) LRC 匹配：本地 .lrc 目录优先 → 网易云 API 兜底 → 无歌词
//  3) 心跳：UDP 50003 每 200ms HEARTBEAT（广播发现设备 → ACK 后单播），ICD V1.2 不变
//  4) 推流：UDP 50004 → 设备  NOW_META(换曲+3s周期) / NOW_PROG(1Hz) / NOW_LRC(回退通道)
//  5) 只读 HTTP :50005（TcpListener 手写，免 URLACL 管理员权限）：
//     GET /cover/<hash>.jpg   240x240 JPEG（≤64KB）
//     GET /lyrics.json?k=<hash>  {"lines":[{"t":ms,"text":"..."}...]}
//  6) 上行接收：50003 收 NOW_CMD（设备遥控播放，预留打印）
//
// 用法：dotnet run [-c Release]   （需与设备同一 WiFi；防火墙放行 UDP 50003/50004、TCP 50005）

using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Windows.Media.Control;

// ---------------- 常量（与 ICD V1.2/V1.3、设备端 pcnow_service.h 一致） ----------------
const int HeartbeatPeriodMs = 200;
const int DeviceOfflineMs   = 5000;
const int SignalPort        = 50003;   // 心跳/信令（双向）
const int NowPort           = 50004;   // PC→设备 NOW_* 推送
const int HttpPort          = 50005;   // 设备→PC 拉取（封面/歌词）
const int CoverSize         = 240;

// 本地歌词搜索目录（可自行增删；递归查找 "<歌手> - <曲名>.lrc"）
string[] LyricDirs =
{
    Environment.GetFolderPath(Environment.SpecialFolder.MyMusic),
    AppContext.BaseDirectory,
};

// ---------------- 命令行参数 ----------------
// --fake            不依赖真实播放器，合成一条"正在播放"的曲目（联调/自检用）
// --device <ip>     跳过广播发现，直接向指定 IP 单播心跳（同机联调填 127.0.0.1）
// --run-seconds <n> 运行 n 秒后自动退出（0=常驻）
bool FakeMode = args.Contains("--fake");
string ForceDeviceIp = GetValue(args, "--device") ?? "";
int RunSeconds = int.TryParse(GetValue(args, "--run-seconds"), out var rs) ? rs : 0;

static string? GetValue(string[] args, string name)
{
    var i = Array.IndexOf(args, name);
    return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
}

// ---------------- 全局状态 ----------------
object Lock = new();
string _deviceIp = "";            // 从 HEARTBEAT_ACK 学到（--device 模式直接预置）
DateTime _lastAck = DateTime.MinValue;
bool _deviceOnline = false;

string _title = "", _artist = "", _album = "", _player = "";
double _durationMs = 0, _lastPosMs = 0;
DateTime _lastPosWall = DateTime.UtcNow;
bool _playing = false;
bool _haveMedia = false;

string _lrcHash = "", _coverHash = "";List<(int t, string text)> _lrcLines = new();
byte[] _coverJpg = Array.Empty<byte>();

GlobalSystemMediaTransportControlsSession? _session = null;

// =====================================================================
// --fake 模拟曲目（联调用：不依赖任何真实播放器）
// =====================================================================
async Task FakeTrackLoopAsync(CancellationToken ct)
{
    lock (Lock)
    {
        _title = "Love Song";
        _artist = "方大同";
        _album = "未来";
        _player = "FAKE";
        _durationMs = 214000;
        _lastPosMs = 0;
        _lastPosWall = DateTime.UtcNow;
        _playing = true;
        _haveMedia = true;
        _lrcLines = new List<(int, string)>
        {
            (0, "写一首简单的歌"),
            (8000, "记录我们的爱"),
            (16000, "旋律在心中流转"),
            (24000, "因为我不想忘记"),
            (32000, "这一刻的画面"),
            (40000, "永远都为你保留"),
        };
        _lrcHash = Sha8(_title + "|" + _artist);
    }
    // 假封面：纯 System.Drawing 画一张 240x240（不需要网易云）
    try
    {
        using var bmp = new System.Drawing.Bitmap(CoverSize, CoverSize);
        using (var g = System.Drawing.Graphics.FromImage(bmp))
        {
            g.Clear(System.Drawing.Color.FromArgb(30, 58, 95));
            using var brush = new System.Drawing.Drawing2D.LinearGradientBrush(
                new System.Drawing.Rectangle(0, 0, CoverSize, CoverSize),
                System.Drawing.Color.FromArgb(34, 211, 238),
                System.Drawing.Color.FromArgb(59, 130, 246), 45f);
            g.FillEllipse(brush, 30, 30, CoverSize - 60, CoverSize - 60);
        }
        using var ms = new MemoryStream();
        bmp.Save(ms, System.Drawing.Imaging.ImageFormat.Jpeg);
        lock (Lock)
        {
            _coverJpg = ms.ToArray();
            _coverHash = Sha8(_title + "|" + _artist);
        }
    }
    catch (Exception ex) { Log("fake cover err: " + ex.Message); }
    Log($"[FAKE] track loaded: 方大同 - Love Song (lrc={_lrcHash} cover={_coverHash})");
    // 进度时钟靠 PosNow() 内插自动推进，无需再打点
    await Task.Delay(Timeout.Infinite, ct);
}

// =====================================================================
// 工具
// =====================================================================
static string Sha8(string s)
{
    var h = SHA1.HashData(Encoding.UTF8.GetBytes(s));
    return Convert.ToHexString(h)[..8].ToLowerInvariant();
}

void Log(string msg) => Console.WriteLine($"[{DateTime.Now:HH:mm:ss.fff}] {msg}");

// =====================================================================
// SMTC 采集
// =====================================================================
static string MapPlayer(string? smtcId)
{
    var id = (smtcId ?? "").ToLowerInvariant();
    if (id.Contains("cloudmusic") || id.Contains("netease")) return "NetEase";
    if (id.Contains("qqmusic")) return "QQ Music";
    if (id.Contains("kugou")) return "KuGou";
    if (id.Contains("kuwo")) return "KuWo";
    if (id.Contains("foobar")) return "foobar2000";
    if (id.Contains("chrome") || id.Contains("msedge") || id.Contains("firefox")) return "Browser";
    return id.Length > 0 ? smtcId! : "PC";
}

async Task InitSmtcAsync()
{
    var mgr = await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
    mgr.CurrentSessionChanged += (_, e) => { BindSession(mgr.GetCurrentSession()); RefreshTrack(); };
    BindSession(mgr.GetCurrentSession());
    RefreshTrack();
    Log("SMTC init ok");
}

void BindSession(GlobalSystemMediaTransportControlsSession? s)
{
    if (_session is not null)
    {
        _session.MediaPropertiesChanged -= OnSessionEvt;
        _session.TimelinePropertiesChanged -= OnSessionEvt;
        _session.PlaybackInfoChanged -= OnSessionEvt;
    }
    _session = s;
    if (_session is not null)
    {
        _session.MediaPropertiesChanged += OnSessionEvt;
        _session.TimelinePropertiesChanged += OnSessionEvt;
        _session.PlaybackInfoChanged += OnSessionEvt;
    }
}

void OnSessionEvt(GlobalSystemMediaTransportControlsSession sender, object args) => RefreshTrack();

async void RefreshTrack()
{
    try
    {
        var s = _session;
        if (s is null)
        {
            lock (Lock) { _haveMedia = false; _playing = false; }
            return;
        }
        var mp = await s.TryGetMediaPropertiesAsync();
        var tl = s.GetTimelineProperties();
        bool playing = s.GetPlaybackInfo().PlaybackStatus ==
                       GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing;

        lock (Lock)
        {
            _title = mp.Title ?? "";
            _artist = mp.Artist ?? "";
            _album = mp.AlbumTitle ?? "";
            _player = MapPlayer(s.SourceAppUserModelId);
            _durationMs = tl.EndTime.TotalMilliseconds;
            _lastPosMs = tl.Position.TotalMilliseconds;
            _lastPosWall = DateTime.UtcNow;
            _playing = playing;
            _haveMedia = _title.Length > 0;
        }
    }
    catch (Exception ex) { Log("SMTC refresh err: " + ex.Message); }
}

double PosNow()
{
    lock (Lock)
    {
        var p = _lastPosMs;
        if (_playing) p += (DateTime.UtcNow - _lastPosWall).TotalMilliseconds;
        if (_durationMs > 0 && p > _durationMs) p = _durationMs;
        return Math.Max(0, p);
    }
}

// =====================================================================
// LRC 匹配（本地目录 → 网易云 API）
// =====================================================================
static List<(int t, string text)> ParseLrc(string lrc)
{
    var list = new List<(int, string)>();
    var rx = new Regex(@"\[(\d{1,2}):(\d{1,2})(?:[.:](\d{1,3}))?\]");
    // 过滤 credits 行（作词/作曲/编曲等，网易云返回里带 00:00~00:02 时间戳）
    var credit = new Regex(
        @"^(作词|作曲|编曲|制作|监制|混音|母带|录音|和声|吉他|贝斯|鼓|键盘|弦乐|配唱|填词|谱曲|OP|SP)");
    foreach (var raw in lrc.Replace("\r", "").Split('\n'))
    {
        var ms = rx.Matches(raw);
        if (ms.Count == 0) continue;
        string text = rx.Replace(raw, "").Trim();
        if (text.Length == 0 || credit.IsMatch(text)) continue;
        foreach (Match m in ms)
        {
            int min = int.Parse(m.Groups[1].Value);
            int sec = int.Parse(m.Groups[2].Value);
            int frac = m.Groups[3].Success ? int.Parse(m.Groups[3].Value) : 0;
            int t = min * 60000 + sec * 1000 + (frac switch { <= 9 => frac * 100, <= 99 => frac * 10, _ => frac });
            list.Add((t, text));
        }
    }
    return list.OrderBy(x => x.Item1).ToList();
}

async Task<string?> NetEaseSearchSongIdAsync(string q)
{
    using var hc = new HttpClient();
    hc.DefaultRequestHeaders.Add("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    hc.DefaultRequestHeaders.Add("Referer", "https://music.163.com");
    var url = $"https://music.163.com/api/search/get?s={Uri.EscapeDataString(q)}&type=1&limit=1";
    var json = await hc.GetStringAsync(url);
    using var doc = JsonDocument.Parse(json);
    if (doc.RootElement.TryGetProperty("result", out var res) &&
        res.TryGetProperty("songs", out var songs) && songs.GetArrayLength() > 0 &&
        songs[0].TryGetProperty("id", out var id))
        return id.GetRawText();
    return null;
}

async Task<string?> NetEaseLrcAsync(string title, string artist)
{
    try
    {
        var id = await NetEaseSearchSongIdAsync($"{artist} {title}");
        if (id is null) return null;
        using var hc = new HttpClient();
        hc.DefaultRequestHeaders.Add("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
        hc.DefaultRequestHeaders.Add("Referer", "https://music.163.com");
        var json = await hc.GetStringAsync(
            $"https://music.163.com/api/song/lyric?os=pc&id={id}&lv=-1&tv=-1");
        using var doc = JsonDocument.Parse(json);
        if (doc.RootElement.TryGetProperty("lrc", out var lrc) &&
            lrc.TryGetProperty("lyric", out var ly))
            return ly.GetString();
    }
    catch (Exception ex) { Log("NetEase lrc err: " + ex.Message); }
    return null;
}

string? FindLocalLrc(string title, string artist)
{
    try
    {
        foreach (var dir in LyricDirs)
        {
            if (dir.Length == 0 || !Directory.Exists(dir)) continue;
            var f = Directory.EnumerateFiles(dir, "*.lrc", SearchOption.AllDirectories)
                .FirstOrDefault(p =>
                {
                    var name = Path.GetFileNameWithoutExtension(p);
                    return name.Contains(title, StringComparison.OrdinalIgnoreCase) &&
                           (artist.Length == 0 || name.Contains(artist, StringComparison.OrdinalIgnoreCase));
                });
            if (f is not null) return File.ReadAllText(f);
        }
    }
    catch (Exception ex) { Log("local lrc err: " + ex.Message); }
    return null;
}

async Task LoadLyricsAsync(string title, string artist)
{
    var lrc = FindLocalLrc(title, artist) ?? await NetEaseLrcAsync(title, artist);
    var lines = lrc is null ? new List<(int, string)>() : ParseLrc(lrc);
    lock (Lock)
    {
        _lrcLines = lines;
        _lrcHash = lines.Count > 0 ? Sha8(title + "|" + artist) : "";
    }
    Log(lines.Count > 0 ? $"lyrics: {lines.Count} lines (hash {_lrcHash})" : "lyrics: none");
}

// =====================================================================
// 封面（网易云 API → SMTC Thumbnail 兜底）→ 240x240 JPEG
// =====================================================================
async Task<byte[]?> NetEaseCoverAsync(string title, string artist)
{
    try
    {
        var id = await NetEaseSearchSongIdAsync($"{artist} {title}");
        if (id is null) return null;
        using var hc = new HttpClient();
        hc.DefaultRequestHeaders.Add("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
        hc.DefaultRequestHeaders.Add("Referer", "https://music.163.com");
        var json = await hc.GetStringAsync($"https://music.163.com/api/song/detail?ids=%5B{id}%5D");
        using var doc = JsonDocument.Parse(json);
        var pic = doc.RootElement.GetProperty("songs")[0].GetProperty("album").GetProperty("picUrl").GetString();
        if (pic is null) return null;
        pic = pic.Replace("http://", "https://");
        return await hc.GetByteArrayAsync(pic);
    }
    catch (Exception ex) { Log("NetEase cover err: " + ex.Message); }
    return null;
}

async Task<byte[]?> SmtcThumbAsync()
{
    try
    {
        var s = _session;
        if (s is null) return null;
        var mp = await s.TryGetMediaPropertiesAsync();
        using var stream = await mp.Thumbnail.OpenReadAsync();
        using var ms = new MemoryStream();
        await stream.AsStreamForRead().CopyToAsync(ms);
        return ms.ToArray();
    }
    catch (Exception ex) { Log("SMTC thumb err: " + ex.Message); }
    return null;
}

byte[]? ResizeJpg(byte[] src, int side)
{
    try
    {
        using var inStream = new MemoryStream(src);
        using var img = System.Drawing.Image.FromStream(inStream);
        using var bmp = new System.Drawing.Bitmap(side, side);
        using var g = System.Drawing.Graphics.FromImage(bmp);
        // 居中裁剪成正方形再缩放（封面通常已是方形，缩放为主）
        int s0 = Math.Min(img.Width, img.Height);
        var srcRect = new System.Drawing.Rectangle((img.Width - s0) / 2, (img.Height - s0) / 2, s0, s0);
        g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
        g.DrawImage(img, new System.Drawing.Rectangle(0, 0, side, side), srcRect, System.Drawing.GraphicsUnit.Pixel);
        using var outStream = new MemoryStream();
        var codec = System.Drawing.Imaging.ImageCodecInfo.GetImageEncoders()
                        .First(c => c.FormatID == System.Drawing.Imaging.ImageFormat.Jpeg.Guid);
        var ep = new System.Drawing.Imaging.EncoderParameters(1);
        ep.Param[0] = new System.Drawing.Imaging.EncoderParameter(System.Drawing.Imaging.Encoder.Quality, 80L);
        bmp.Save(outStream, codec, ep);
        return outStream.ToArray();
    }
    catch (Exception ex) { Log("resize err: " + ex.Message); }
    return null;
}

async Task LoadCoverAsync(string title, string artist)
{
    var jpg = await NetEaseCoverAsync(title, artist) ?? await SmtcThumbAsync();
    var resized = jpg is null ? null : ResizeJpg(jpg, CoverSize);
    lock (Lock)
    {
        _coverJpg = resized ?? Array.Empty<byte>();
        _coverHash = _coverJpg.Length > 0 ? Sha8(title + "|" + artist) : "";
    }
    Log(resized is not null ? $"cover ready: {resized.Length} B (hash {_coverHash})" : "cover: none");
}

// =====================================================================
// UDP：心跳（50003）+ 推流（50004）
// =====================================================================
async Task HeartbeatLoopAsync(CancellationToken ct)
{
    using var udp = new UdpClient();
    udp.EnableBroadcast = true;
    var sw = Stopwatch.StartNew();
    while (!ct.IsCancellationRequested)
    {
        bool unicast;
        string target;
        lock (Lock)
        {
            if (ForceDeviceIp.Length > 0)
            {
                // --device 模式：直接单播，收到 ACK 前也视为目标在线（联调用）
                target = ForceDeviceIp;
                if (_deviceIp.Length == 0)
                {
                    _deviceIp = ForceDeviceIp;
                    _lastAck = DateTime.UtcNow;
                    _deviceOnline = true;
                }
                unicast = true;
            }
            else
            {
                unicast = _deviceIp.Length > 0 && (DateTime.UtcNow - _lastAck).TotalMilliseconds < DeviceOfflineMs;
                target = unicast ? _deviceIp : "255.255.255.255";
            }
        }
        var msg = Encoding.UTF8.GetBytes(
            "{\"type\":\"HEARTBEAT\",\"sw_ver\":\"pcnow_v1\",\"listen_ports\":[" + HttpPort + "]}");
        await udp.SendAsync(msg, msg.Length, target, SignalPort);
        // 收 ACK（顺带收 NOW_CMD 上行）
        while (udp.Available > 0)
        {
            var ra = new IPEndPoint(IPAddress.Any, 0);
            var data = udp.Receive(ref ra);
            var s = Encoding.UTF8.GetString(data);
            if (s.Contains("HEARTBEAT_ACK"))
            {
                bool wasOffline;
                lock (Lock)
                {
                    wasOffline = !_deviceOnline;
                    _deviceIp = ra.Address.ToString();
                    _lastAck = DateTime.UtcNow;
                    _deviceOnline = true;
                }
                if (wasOffline) Log("device ONLINE at " + ra.Address);
            }
            else if (s.Contains("NOW_CMD"))
            {
                Log("NOW_CMD from device: " + s);   // 预留：接 SMTC 控制按钮
            }
        }
        lock (Lock)
        {
            if (_deviceOnline && (DateTime.UtcNow - _lastAck).TotalMilliseconds >= DeviceOfflineMs)
            {
                _deviceOnline = false;
                Log("device OFFLINE (ack timeout)");
            }
        }
        await Task.Delay(HeartbeatPeriodMs, ct);
    }
}

async Task PushLoopAsync(CancellationToken ct)
{
    using var udp = new UdpClient();
    var swMeta = Stopwatch.StartNew();
    var swProg = Stopwatch.StartNew();
    string lastMetaKey = "", lastLrc = "";
    while (!ct.IsCancellationRequested)
    {
        string ip; bool online;
        lock (Lock) { ip = _deviceIp; online = _deviceOnline; }
        if (online && ip.Length > 0)
        {
            string title, artist, album, player, lrcHash, coverHash;
            double dur; bool have, playing;
            lock (Lock)
            {
                title = _title; artist = _artist; album = _album; player = _player;
                dur = _durationMs; have = _haveMedia; playing = _playing;
                lrcHash = _lrcHash; coverHash = _coverHash;
            }

            if (have)
            {
                // NOW_META：换曲立即推 + 每 3s 重发（UDP 容丢）
                string key = title + "|" + artist;
                if (key != lastMetaKey || swMeta.ElapsedMilliseconds >= 3000)
                {
                    var meta = JsonSerializer.Serialize(new
                    {
                        type = "NOW_META",
                        title, artist, album, player,
                        duration_ms = (int)dur,
                        lrc = lrcHash,
                        cover = coverHash,
                    });
                    await SendUdpAsync(udp, ip, NowPort, meta);
                    lastMetaKey = key;
                    swMeta.Restart();
                }

                // NOW_PROG：1Hz
                if (swProg.ElapsedMilliseconds >= 1000)
                {
                    var pos = PosNow();
                    var prog = JsonSerializer.Serialize(new
                    {
                        type = "NOW_PROG",
                        pos_ms = (int)pos,
                        playing,
                    });
                    await SendUdpAsync(udp, ip, NowPort, prog);
                    swProg.Restart();
                }

                // NOW_LRC（v1 回退通道）：行变化才推，与 /lyrics.json 并存
                List<(int t, string text)> lines; string lh;
                lock (Lock) { lines = _lrcLines; lh = _lrcHash; }
                if (lines.Count > 0 && lh.Length > 0)
                {
                    int pos = (int)PosNow();
                    int idx = -1;
                    for (int i = 0; i < lines.Count; i++)
                        if (lines[i].t <= pos) idx = i; else break;
                    string cur = idx >= 0 ? lines[idx].text : "";
                    if (cur != lastLrc)
                    {
                        var lmsg = JsonSerializer.Serialize(new { type = "NOW_LRC", text = cur, index = idx });
                        await SendUdpAsync(udp, ip, NowPort, lmsg);
                        lastLrc = cur;
                    }
                }
            }
        }
        await Task.Delay(50, ct);
    }
}

static async Task SendUdpAsync(UdpClient udp, string ip, int port, string payload)
{
    var b = Encoding.UTF8.GetBytes(payload);
    await udp.SendAsync(b, b.Length, ip, port);
}

// =====================================================================
// 只读 HTTP :50005（TcpListener 手写，免 URLACL）
// =====================================================================
async Task HttpLoopAsync(CancellationToken ct)
{
    var listener = new TcpListener(IPAddress.Any, HttpPort);
    listener.Start();
    Log($"http listening on :{HttpPort} (/cover/<hash>.jpg, /lyrics.json?k=<hash>)");
    while (!ct.IsCancellationRequested)
    {
        var client = await listener.AcceptTcpClientAsync(ct);
        _ = Task.Run(() => ServeOne(client), ct);
    }
}

void ServeOne(TcpClient client)
{
    try
    {
        using var _ = client;
        using var stream = client.GetStream();
        var buf = new byte[4096];
        int n = stream.Read(buf, 0, buf.Length);
        if (n <= 0) return;
        var req = Encoding.UTF8.GetString(buf, 0, n).Split("\r\n")[0];   // "GET /path HTTP/1.1"
        var path = req.Split(' ').ElementAtOrDefault(1) ?? "/";
        var url = Uri.UnescapeDataString(path);

        byte[] body; string ctype;
        if (url.StartsWith("/cover/"))
        {
            var want = url.Split('/').LastOrDefault()?.Replace(".jpg", "");
            lock (Lock)
            {
                body = want == _coverHash ? _coverJpg : Array.Empty<byte>();
                ctype = "image/jpeg";
            }
        }
        else if (url.StartsWith("/lyrics.json"))
        {
            var want = "";
            var qi = url.IndexOf("k=");
            if (qi >= 0) want = url[(qi + 2)..].Split('&')[0];
            List<(int t, string text)> lines;
            lock (Lock) { lines = want == _lrcHash ? _lrcLines : new(); }
            var arr = lines.Select(l => new { t = l.t, text = l.text });
            body = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new { lines = arr }));
            ctype = "application/json";
        }
        else
        {
            body = Encoding.UTF8.GetBytes("{\"svc\":\"pcnow\",\"ok\":true}");
            ctype = "application/json";
        }

        var head = Encoding.UTF8.GetBytes(
            $"HTTP/1.1 {(body.Length > 0 || ctype == "application/json" ? 200 : 404)} OK\r\n" +
            $"Content-Type: {ctype}\r\nContent-Length: {body.Length}\r\nConnection: close\r\n\r\n");
        stream.Write(head, 0, head.Length);
        stream.Write(body, 0, body.Length);
    }
    catch { /* 客户端断开等，忽略 */ }
}

// =====================================================================
// 换曲触发：拉歌词 + 封面（后台，不阻塞推流）
// =====================================================================
async Task TrackWorkerAsync(CancellationToken ct)
{
    string last = "";
    while (!ct.IsCancellationRequested)
    {
        string title, artist; bool have;
        lock (Lock) { title = _title; artist = _artist; have = _haveMedia; }
        string key = have ? title + "|" + artist : "";
        if (key.Length > 0 && key != last)
        {
            last = key;
            Log($"track: {artist} - {title}");
            await LoadLyricsAsync(title, artist);
            await LoadCoverAsync(title, artist);
        }
        else if (key.Length == 0)
        {
            last = "";
            lock (Lock) { _lrcLines = new(); _lrcHash = ""; _coverJpg = Array.Empty<byte>(); _coverHash = ""; }
        }
        await Task.Delay(500, ct);
    }
}

// =====================================================================
// main
// =====================================================================
var ct = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; ct.Cancel(); };

Task fakeTask = Task.CompletedTask;
if (FakeMode)
{
    Log("FAKE mode: 合成曲目（不依赖真实播放器）");
    fakeTask = FakeTrackLoopAsync(ct.Token);
}
else
{
    await InitSmtcAsync();
}

var hb = HeartbeatLoopAsync(ct.Token);
var push = PushLoopAsync(ct.Token);
var http = HttpLoopAsync(ct.Token);
var track = FakeMode ? Task.CompletedTask : TrackWorkerAsync(ct.Token);
Log($"pc_now_agent started. Ctrl+C to exit.{(RunSeconds > 0 ? $" (auto exit in {RunSeconds}s)" : "")}");

List<Task> all = new() { hb, push, http, track, fakeTask };
try
{
    if (RunSeconds > 0)
    {
        // ★ 到点主动 Cancel 再 WhenAll（原写法只是"多等一个定时器"，进程永不自退）
        _ = Task.Run(async () =>
        {
            await Task.Delay(TimeSpan.FromSeconds(RunSeconds));
            ct.Cancel();
        });
    }
    await Task.WhenAll(all);
}
catch (OperationCanceledException) { /* --run-seconds 到点或 Ctrl+C */ }
catch (Exception ex)
{
    Log("FATAL: " + ex);
}
ct.Cancel();
Log("pc_now_agent exited.");
