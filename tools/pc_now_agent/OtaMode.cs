// SPDX-License-Identifier: Apache-2.0
// OtaMode.cs — pc_now_agent 的 --ota 模式（WiFi 局域网 OTA 升级，方案 §4）
//
// 职责：
//  1) 解析 app bin 元信息：esp_app_desc_t（偏移 0x20，magic 0xABCD5432）→ version；
//     现算文件大小与 SHA256
//  2) HTTP :50005 新增两条路由：
//       GET /firmware.json  → {"ver","size","sha256"}
//       GET /firmware.bin   → 200/206 + Content-Length + 分段写出（支持 Range）
//  3) 会话：等心跳确认设备在线 → 版本比对 → 发 OTA_OFFER（UDP 50003）
//     → 等 ACCEPT/REJECT → 收 PROGRESS 打印进度条 → 收 RESULT 判定
//  4) 破坏性测试开关（联调/验收用）：
//       --ota-limit N    只发前 N%（触发设备端 size mismatch，验证续传）
//       --ota-corrupt    固件流中间翻一个 bit（触发 esp_ota_end / 段哈希失败）
//       --force          忽略版本不升（同版本/降级也照发）
//  5) 掉线检测：会话期间设备心跳丢失 > 8s ⇒ 主动发 OTA_ABORT 让设备停手
//
// ★ "就绪才宣告"纪律（F44 同源）：/firmware.bin 只有在固件真的解析成功后
//   才注册到路由表；解析失败直接退出，绝不注册半成品路由。

using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace PcNowAgent;

/// <summary>--ota 模式的入口与全局句柄。
/// Active 由主流程在解析成功后写入；HTTP 路由据此决定 /firmware* 是否存在
/// （★ "就绪才宣告"：解析失败 ⇒ Active 为 null ⇒ 路由不注册，设备拿不到半成品）。</summary>
internal static class OtaMode
{
    /// <summary>当前 OTA 会话（null = 非 OTA 模式或固件解析失败）。</summary>
    public static OtaSession? Active { get; set; }
}

/// <summary>OTA 破坏性测试 / 行为开关（来自命令行）。</summary>
internal sealed class OtaOptions
{
    /// <summary>--ota-limit N：只发前 N%（1..99）。0 = 全量。</summary>
    public int LimitPercent;
    /// <summary>--ota-corrupt：在固件流中间翻一个 bit。</summary>
    public bool Corrupt;
    /// <summary>--force：忽略版本比对（同版本/降级也照发）。</summary>
    public bool Force;
}

/// <summary>固件 bin 的元信息（从 esp_app_desc_t 解析，方案 §4.1）。</summary>
internal sealed class FirmwareInfo
{
    public string Path = "";
    public string Version = "";
    public string ProjectName = "";
    public string BuildTime = "";
    public long Size;
    public string Sha256 = "";

    private const uint AppDescMagic = 0xABCD5432;
    // esp_image_header(24B) + esp_image_segment_header(8B) = 32B，之后是 esp_app_desc_t
    private const int DescOffset = 0x20;

    /// <summary>解析失败返回 null（挡住误拖 bootloader.bin / partition-table.bin）。</summary>
    public static FirmwareInfo? Parse(string path)
    {
        try
        {
            var bytes = File.ReadAllBytes(path);
            if (bytes.Length < DescOffset + 256)
            {
                Console.WriteLine($"[ota] 文件太小（{bytes.Length} B），不是 app bin");
                return null;
            }
            uint magic = BitConverter.ToUInt32(bytes, DescOffset);
            if (magic != AppDescMagic)
            {
                Console.WriteLine($"[ota] magic 不对（0x{magic:X8} != 0x{AppDescMagic:X8}）——"
                                  + "这不是 app bin（是不是误选了 bootloader.bin / partition-table.bin？）");
                return null;
            }
            var info = new FirmwareInfo
            {
                Path = path,
                Size = bytes.LongLength,
                Version = ReadCString(bytes, DescOffset + 16, 32),   // version[32]
                ProjectName = ReadCString(bytes, DescOffset + 48, 32),   // project_name[32]
                BuildTime = ReadCString(bytes, DescOffset + 80, 16),   // time[16]
                Sha256 = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(),
            };
            return info;
        }
        catch (Exception ex)
        {
            Console.WriteLine("[ota] 解析失败: " + ex.Message);
            return null;
        }
    }

    private static string ReadCString(byte[] b, int off, int max)
    {
        int end = off;
        while (end < off + max && end < b.Length && b[end] != 0) end++;
        return Encoding.ASCII.GetString(b, off, end - off).Trim();
    }
}

/// <summary>OTA 会话（方案 §4.4）。持有固件元信息，供 HTTP 路由读取。</summary>
internal sealed class OtaSession
{
    private readonly FirmwareInfo _fw;
    private readonly OtaOptions _opt;
    private readonly int _httpPort;
    private readonly int _signalPort;
    private readonly Func<string> _getDeviceIp;
    private readonly Func<bool> _isDeviceOnline;

    // 结果统计（HTTP 侧写、会话侧读）
    public long BytesServed;
    public int ServeCount;

    public OtaSession(FirmwareInfo fw, OtaOptions opt, int httpPort, int signalPort,
                      Func<string> getDeviceIp, Func<bool> isDeviceOnline)
    {
        _fw = fw;
        _opt = opt;
        _httpPort = httpPort;
        _signalPort = signalPort;
        _getDeviceIp = getDeviceIp;
        _isDeviceOnline = isDeviceOnline;
    }

    public FirmwareInfo Firmware => _fw;
    public OtaOptions Options => _opt;

    /// <summary>设备真正能下到的字节数（--ota-limit 生效时小于文件大小）。</summary>
    public long ServedSize =>
        _opt.LimitPercent > 0 ? _fw.Size * _opt.LimitPercent / 100 : _fw.Size;

    /// <summary>GET /firmware.json → {"ver","size","sha256"}（设备二次核对用）。
    /// ★ --ota-limit 时必须上报**截断后**的 size —— 否则设备会按整包长度判定，
    ///   直接落到 "size mismatch"（那正是该开关的意图，但要让语义明确）。</summary>
    public byte[] FirmwareJson()
    {
        return Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new
        {
            ver = _fw.Version,
            size = ServedSize,
            sha256 = _fw.Sha256,
            full_size = _fw.Size,
            limit_percent = _opt.LimitPercent,
        }));
    }

    /// <summary>GET /firmware.bin → 流式写出（64KB 分块，避免整包进内存）。
    /// 支持 HTTP Range（断点续传，方案 §4.3）。</summary>
    /// <param name="outStream">响应体输出流</param>
    /// <param name="startOffset">Range 起始偏移（0 = 整包）</param>
    /// <param name="isHead">true = 只写头，不打 body（HEAD 请求）</param>
    public void ServeFirmwareBinary(Stream outStream, long startOffset, bool isHead)
    {
        const int Chunk = 64 * 1024;
        long limit = ServedSize;                 // 本次要发的总字节数（截断口径）
        if (startOffset < 0) startOffset = 0;
        if (startOffset > limit) startOffset = limit;
        long remain = limit - startOffset;

        if (isHead)
        {
            Console.WriteLine($"[ota] HEAD firmware.bin: total={limit} B, range={startOffset}-");
            return;
        }

        using var fs = File.OpenRead(_fw.Path);
        fs.Seek(startOffset, SeekOrigin.Begin);
        var buf = new byte[Chunk];
        long total = 0;
        // --ota-corrupt：在整个流的中点翻一个 bit
        long corruptAt = _opt.Corrupt ? (limit - startOffset) / 2 : -1;
        bool corrupted = false;

        while (total < remain)
        {
            int want = (int)Math.Min(buf.Length, remain - total);
            int n = fs.Read(buf, 0, want);
            if (n <= 0) break;

            if (corruptAt >= 0 && !corrupted && total + n > corruptAt)
            {
                int idx = (int)(corruptAt - total);
                buf[idx] ^= 0x01;
                corrupted = true;
                Console.WriteLine($"[ota] ★ CORRUPT injected at stream offset {startOffset + corruptAt} "
                                  + "(1 bit flipped) —— 预期设备端 esp_ota_end / 段哈希报错");
            }

            outStream.Write(buf, 0, n);
            total += n;
        }

        lock (_lock)
        {
            BytesServed += total;
            ServeCount++;
        }
        Console.WriteLine($"[ota] served firmware.bin: {total} B "
                          + $"(offset={startOffset}, {(_opt.LimitPercent > 0 ? $"limit={_opt.LimitPercent}%" : "full")}"
                          + $"{(_opt.Corrupt ? ", CORRUPTED" : "")}) 请求 #{ServeCount}");
    }

    /// <summary>会话主流程：等设备 → 版本比对 → OFFER → ACCEPT → PROGRESS → RESULT。</summary>
    public async Task<int> RunAsync(CancellationToken ct)
    {
        Console.WriteLine();
        Console.WriteLine("================== OTA 升级会话 ==================");
        Console.WriteLine($"  固件    : {_fw.Path}");
        Console.WriteLine($"  版本    : {_fw.Version}  ({_fw.ProjectName}, build {_fw.BuildTime})");
        Console.WriteLine($"  大小    : {_fw.Size} B ({_fw.Size / 1024.0 / 1024.0:F2} MB)");
        Console.WriteLine($"  SHA256  : {_fw.Sha256}");
        Console.WriteLine($"  下载 URL: http://<本机>:{_httpPort}/firmware.bin");
        if (_opt.LimitPercent > 0)
            Console.WriteLine($"  ★ 测试开关: --ota-limit {_opt.LimitPercent}% → 只发 {ServedSize} B");
        if (_opt.Corrupt)
            Console.WriteLine("  ★ 测试开关: --ota-corrupt → 流中点翻 1 bit");
        if (_opt.Force)
            Console.WriteLine("  ★ 测试开关: --force → 忽略版本比对");
        Console.WriteLine("==================================================");

        // 等设备上线（心跳 ACK）
        Console.Write("[ota] 等待设备心跳");
        var t0 = DateTime.UtcNow;
        while (!_isDeviceOnline() && (DateTime.UtcNow - t0).TotalSeconds < 20)
        {
            Console.Write(".");
            await Task.Delay(500, ct);
        }
        Console.WriteLine();
        if (!_isDeviceOnline())
        {
            Console.WriteLine("[ota] 设备未上线（20s 超时）。检查：同一 WiFi？防火墙放行 UDP 50003？");
            return 2;
        }
        string devIp = _getDeviceIp();
        Console.WriteLine($"[ota] 设备在线: {devIp}");
        Console.WriteLine($"[ota] 设备当前版本: {DeviceFwVer ?? "(未上报)"}");

        // ★ 版本比对（SemVer 粗比）：设备版本 >= 固件版本 ⇒ 无意义升级，拒绝执行
        if (!_opt.Force && DeviceFwVer is { Length: > 0 } dv)
        {
            if (CompareSemVer(dv, _fw.Version) >= 0)
            {
                Console.WriteLine($"[ota] ★ 版本未提升（设备 {dv} >= 固件 {_fw.Version}），"
                                  + "默认不发 OFFER。要强行刷入请加 --force。");
                return 8;
            }
        }
        else if (!_opt.Force && DeviceFwVer is null or { Length: 0 })
        {
            Console.WriteLine("[ota] 设备未上报 fw_ver，跳过版本比对（继续）。");
        }

        // 本机在设备侧看到的 IP：设备会用 OFFER 里的 url 去拉，必须是设备能到达的地址。
        var localIp = PickLocalIpFor(devIp);
        var url = $"http://{localIp}:{_httpPort}/firmware.bin";
        Console.WriteLine($"[ota] 通告 URL: {url}");

        using var udp = new UdpClient();
        // ★ size 上报 ServedSize：--ota-limit 时设备会按截断长度判定（正是测试意图）
        var offer = JsonSerializer.Serialize(new
        {
            type = "OTA_OFFER",
            ver = _fw.Version,
            size = ServedSize,
            sha256 = _fw.Sha256,
            url,
        });

        // OFFER 10s×3 重发（方案 §7：UDP 丢包对策）
        bool accepted = false;
        for (int attempt = 1; attempt <= 3 && !accepted; attempt++)
        {
            Console.WriteLine($"[ota] 发送 OTA_OFFER（第 {attempt}/3 次）");
            var ob = Encoding.UTF8.GetBytes(offer);
            await udp.SendAsync(ob, ob.Length, devIp, _signalPort);

            // 等 ACCEPT/REJECT（最多 10s），同时消费 PROGRESS/RESULT
            var wait = DateTime.UtcNow;
            while ((DateTime.UtcNow - wait).TotalSeconds < 10)
            {
                if (ct.IsCancellationRequested) return 130;
                using var timeout = new CancellationTokenSource(500);
                try
                {
                    var r = await udp.ReceiveAsync(timeout.Token);
                    var s = Encoding.UTF8.GetString(r.Buffer);
                    if (s.Contains("OTA_ACCEPT"))
                    {
                        Console.WriteLine("[ota] 设备已接受，开始下载...");
                        accepted = true;
                        break;
                    }
                    if (s.Contains("OTA_REJECT"))
                    {
                        Console.WriteLine($"[ota] 设备拒绝: {s}");
                        Console.WriteLine("       reason 含义: busy=录音/通话中  pending=待自证  "
                                          + "space=槽不够  network=离线  badargs/badjson=参数问题");
                        // ★ 被拒也回一个明确退出码，便于脚本判定
                        return 3;
                    }
                }
                catch (OperationCanceledException) { /* 本轮无包，继续等 */ }
            }
        }
        if (!accepted)
        {
            Console.WriteLine("[ota] 未收到 OTA_ACCEPT（3 次 OFFER 已用尽）。");
            return 4;
        }

        // 下载/校验阶段：收 PROGRESS 画进度条，收 RESULT 收尾（总超时 180s）
        int lastPct = -1;
        bool abortSent = false;
        var dl0 = DateTime.UtcNow;
        var lastSeenOnline = DateTime.UtcNow;
        while ((DateTime.UtcNow - dl0).TotalSeconds < 180)
        {
            if (ct.IsCancellationRequested) return 130;

            // ★ 掉线检测：设备心跳丢失 > 8s ⇒ 主动 OTA_ABORT（设备侧块边界立即停）
            if (_isDeviceOnline())
            {
                lastSeenOnline = DateTime.UtcNow;
            }
            else if (!abortSent && (DateTime.UtcNow - lastSeenOnline).TotalSeconds > 8)
            {
                abortSent = true;
                var ab = Encoding.UTF8.GetBytes("{\"type\":\"OTA_ABORT\",\"reason\":\"peer_lost\"}");
                try { await udp.SendAsync(ab, ab.Length, devIp, _signalPort); } catch { }
                Console.WriteLine("\n[ota] ★ 设备心跳丢失 >8s，已发送 OTA_ABORT（设备应立即停手）");
            }

            using var timeout = new CancellationTokenSource(1000);
            try
            {
                var r = await udp.ReceiveAsync(timeout.Token);
                var s = Encoding.UTF8.GetString(r.Buffer);
                if (s.Contains("OTA_PROGRESS"))
                {
                    var pct = ExtractInt(s, "percent", -1);
                    var speed = ExtractInt(s, "speed", 0);
                    if (pct >= 0 && pct != lastPct)
                    {
                        lastPct = pct;
                        Console.Write($"\r[ota] {ProgressBar(pct)} {pct,3}%  {speed} KB/s   ");
                    }
                }
                else if (s.Contains("OTA_RESULT"))
                {
                    Console.WriteLine();
                    if (s.Contains("\"ok\":1"))
                    {
                        Console.WriteLine("[ota] 设备上报：升级成功，正在重启…");
                        Console.WriteLine("[ota] 等待设备以新版本回来（自证 30s 内）…");
                        var r0 = DateTime.UtcNow;
                        while ((DateTime.UtcNow - r0).TotalSeconds < 60)
                        {
                            await Task.Delay(1000, ct);
                            if (_isDeviceOnline())
                            {
                                Console.WriteLine($"[ota] 设备已回来，IP={_getDeviceIp()}，"
                                                  + $"fw_ver={DeviceFwVer}");
                                bool verOk = DeviceFwVer == _fw.Version;
                                Console.WriteLine($"[ota] ★ 版本核对: 期望 {_fw.Version} / 实际 {DeviceFwVer} "
                                                  + (verOk ? "→ 升级成功 ✅" : "→ 不一致 ❌（可能已回滚）"));
                                return verOk ? 0 : 9;
                            }
                        }
                        Console.WriteLine("[ota] 设备重启后 60s 内未恢复心跳 —— 可能已自动回滚，"
                                          + "请查串口日志（rollback 关键字）");
                        return 5;
                    }
                    else
                    {
                        Console.WriteLine($"[ota] 设备上报：升级失败 → {s}");
                        return 6;
                    }
                }
            }
            catch (OperationCanceledException) { /* 本轮无包 */ }
        }
        Console.WriteLine("\n[ota] 下载/校验阶段 180s 超时（设备可能已断开）。");
        return 7;
    }

    // ---- 设备当前版本：由 HeartbeatLoop 从 HEARTBEAT_ACK.fw_ver 更新（见 Program.cs）----
    /// <summary>设备最新上报的固件版本（null/空 = 未上报）。</summary>
    public static string? DeviceFwVer { get; set; }

    // ---- 会话状态锁（HTTP 侧字节统计用；会话侧只读）----
    private readonly object _lock = new();

    private static string ProgressBar(int pct)
    {
        int bars = Math.Clamp(pct / 2, 0, 50);
        return "[" + new string('#', bars) + new string('-', 50 - bars) + "]";
    }

    /// <summary>SemVer 粗比对：a>b 返回正、a&lt;b 返回负、相等返回 0。
    /// 支持 "1.2.3" / "1.2" / "v1.2.3" / 带 "-dirty" 后缀。
    /// ★ 只用于"要不要升"的粗判；真正的完整性靠 SHA256，版本号不参与安全判定。</summary>
    public static int CompareSemVer(string a, string b)
    {
        static int[] Parse(string s)
        {
            s = s.Trim().TrimStart('v', 'V');
            var core = s.Split('-', '+')[0];            // 丢掉 -dirty / +build
            var parts = core.Split('.');
            var n = new int[3];
            for (int i = 0; i < 3 && i < parts.Length; i++)
                int.TryParse(parts[i], out n[i]);
            return n;
        }
        var (x, y) = (Parse(a), Parse(b));
        for (int i = 0; i < 3; i++)
        {
            if (x[i] != y[i]) return x[i] - y[i];
        }
        return 0;
    }

    private static int ExtractInt(string json, string key, int dflt)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            if (doc.RootElement.TryGetProperty(key, out var v) && v.TryGetInt32(out var i))
                return i;
        }
        catch { }
        return dflt;
    }

    /// <summary>选一个设备能到达的本机 IPv4（与设备同网段的那个网卡）。</summary>
    private static string PickLocalIpFor(string deviceIp)
    {
        try
        {
            var target = IPAddress.Parse(deviceIp);
            // 连一个不会真发包的 UDP "连接"来让内核选路由，从而得到出接口 IP
            using var probe = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
            probe.Connect(new IPEndPoint(target, 9));   // discard 端口，不会真发数据
            return ((IPEndPoint)probe.LocalEndPoint!).Address.ToString();
        }
        catch
        {
            return "127.0.0.1";
        }
    }
}
