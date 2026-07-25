using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

namespace CipriansBridge;

/// <summary>
/// One running `ctm-usbip bt` child = one bridged device. The child hosts its
/// own USB/IP server and auto-attaches; stopping it unplugs the virtual
/// device. Profile/map selection stays DATA-driven: curated files for DS4/DS5,
/// `--profile auto` (identity pass-through) for everything else — the app adds
/// no controller knowledge of its own.
/// </summary>
public class PlugSession
{
    private readonly string _exe;
    private readonly DeviceVm _device;
    private readonly Action<string> _log;
    private Process? _process;

    public int Slot { get; }
    public string BusId { get; }
    public int UsbipPort { get; }
    public string? HiddenInstanceId { get; set; }
    public bool HasExited => _process is { HasExited: true };

    public PlugSession(string exePath, DeviceVm device, int slot, Action<string> log)
    {
        _exe = exePath;
        _device = device;
        _log = log;
        Slot = slot;
        BusId = $"ciprian-{slot + 1}";
        UsbipPort = 3250 + slot;   // keep clear of the agent's own server on 3240
    }

    /// <summary>Maps (and profiles) prefer a LIVE-EDIT override under
    /// %ProgramData%\CTM Bridge\ over the installed copy: every Plug in loads
    /// the map fresh from disk, so edit the override file, Plug out, Plug in —
    /// no rebuild, no reinstall. The session log's "map:" line shows which
    /// file was loaded.</summary>
    private static string OverridableAsset(string exeDir, string relative)
    {
        var overridePath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
            "CTM Bridge", relative);
        return File.Exists(overridePath) ? overridePath : Path.Combine(exeDir, relative);
    }

    public bool Start()
    {
        var exeDir = Path.GetDirectoryName(_exe)!;
        var args = $"bt {_device.Index} --busid {BusId} --usbip-port {UsbipPort}";
        switch (_device.DeviceType)
        {
            case "ds4_bt":
                args += $" --profile \"{OverridableAsset(exeDir, Path.Combine("profiles", "descriptors", "ds4_composite.profile"))}\"" +
                        $" --map \"{OverridableAsset(exeDir, Path.Combine("maps", "ds4_usb_over_ds4_bt.map"))}\"";
                break;
            case "ds5_bt":
                break;   // ctm-usbip's built-in default IS the DS5 pair
            default:
                args += " --profile auto";
                break;
        }

        var psi = new ProcessStartInfo(_exe, args)
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
            WorkingDirectory = exeDir,
        };
        // Debug phase: full per-endpoint logging, teed to a per-session file so
        // "went dead mid-game" is diagnosable after the fact.
        psi.EnvironmentVariables["CTM_USBIP_VERBOSE"] = "1";
        var logDir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "CTM Bridge");
        Directory.CreateDirectory(logDir);
        var logPath = Path.Combine(logDir, $"ciprians-{BusId}.log");
        var logWriter = new StreamWriter(logPath, append: false) { AutoFlush = true };
        logWriter.WriteLine($"=== {DateTime.Now:yyyy-MM-dd HH:mm:ss} {_exe} {args}");
        void Tee(string line)
        {
            lock (logWriter) logWriter.WriteLine(line);
            _log($"[{BusId}] {line}");
        }
        try
        {
            _process = Process.Start(psi);
            if (_process == null) { logWriter.Dispose(); return false; }
            _process.EnableRaisingEvents = true;
            _process.Exited += (_, _) =>
            {
                try { lock (logWriter) logWriter.WriteLine($"=== child exited code {_process?.ExitCode}"); } catch { }
                try { logWriter.Dispose(); } catch { }
            };
            _process.OutputDataReceived += (_, e) => { if (!string.IsNullOrWhiteSpace(e.Data)) Tee(e.Data); };
            _process.ErrorDataReceived += (_, e) => { if (!string.IsNullOrWhiteSpace(e.Data)) Tee(e.Data); };
            _process.BeginOutputReadLine();
            _process.BeginErrorReadLine();
            // Give the child a moment to open the device + start its server;
            // an immediate exit means the plug failed (bad index, open error).
            if (_process.WaitForExit(1500)) return false;
            return true;
        }
        catch (Exception ex)
        {
            _log($"start failed: {ex.Message}");
            return false;
        }
    }

    /// <summary>Graceful Ctrl+Break first (the CLI handles it and detaches
    /// cleanly), hard kill as fallback.</summary>
    public void Stop()
    {
        var p = _process;
        if (p == null || p.HasExited) return;
        try
        {
            if (AttachConsole((uint)p.Id))
            {
                // Group 0 = EVERY process on this console, including us — a
                // WPF app's default CTRL_BREAK action is exit, so a real
                // handler must swallow it for the duration (null+TRUE only
                // ignores CTRL_C).
                SetConsoleCtrlHandler(SwallowCtrl, true);
                GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
                var exited = p.WaitForExit(3000);
                FreeConsole();
                SetConsoleCtrlHandler(SwallowCtrl, false);
                if (exited) return;
            }
        }
        catch { /* fall through to kill */ }
        try { p.Kill(entireProcessTree: true); p.WaitForExit(2000); } catch { }
    }

    // Static so the delegate stays alive while registered (GC safety).
    private static readonly HandlerRoutine SwallowCtrl = _ => true;

    private const uint CTRL_BREAK_EVENT = 1;

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool AttachConsole(uint dwProcessId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FreeConsole();

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GenerateConsoleCtrlEvent(uint dwCtrlEvent, uint dwProcessGroupId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetConsoleCtrlHandler(HandlerRoutine? handler, bool add);

    private delegate bool HandlerRoutine(uint ctrlType);
}

/// <summary>
/// Thin wrapper over the already-installed HidHide (Nefarius) CLI: hide the
/// physical pad from games while its virtual clone is plugged, with
/// ctm-usbip.exe whitelisted so the bridge keeps reading it.
/// </summary>
public class HidHide
{
    private readonly string? _cli;

    public HidHide()
    {
        var candidates = new[]
        {
            @"C:\Program Files\Nefarius Software Solutions\HidHide\x64\HidHideCLI.exe",
            @"C:\Program Files\Nefarius Software Solutions\HidHide\HidHideCLI.exe",
        };
        _cli = candidates.FirstOrDefault(File.Exists);
    }

    public bool Available => _cli != null;

    public void RegisterApplication(string exePath)
    {
        Run($"--app-reg \"{exePath}\"");
    }

    public bool Hide(string instanceId)
    {
        return Run($"--dev-hide \"{instanceId}\"") && Run("--cloak-on");
    }

    public void Unhide(string instanceId)
    {
        Run($"--dev-unhide \"{instanceId}\"");
    }

    private bool Run(string args)
    {
        if (_cli == null) return false;
        try
        {
            var psi = new ProcessStartInfo(_cli, args)
            {
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            };
            using var p = Process.Start(psi);
            if (p == null) return false;
            p.WaitForExit(8000);
            return p.ExitCode == 0;
        }
        catch
        {
            return false;
        }
    }
}
