using System.ComponentModel;
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Media;
using System.Windows.Threading;

namespace CipriansBridge;

public partial class MainWindow : Window
{
    private readonly CtmCli _cli = new();
    private readonly HidHide _hidHide = new();
    private readonly Dictionary<string, PlugSession> _sessions = new();  // key = instance_id
    private readonly Dictionary<string, DeviceVm> _byKey = new();
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromSeconds(3) };
    private bool _busy;

    public MainWindow()
    {
        InitializeComponent();
        if (_cli.ExePath == null)
        {
            LogText.Text = "ctm-usbip.exe not found (install CTM Bridge or build Release)";
        }
        HidHideText.Text = _hidHide.Available ? "HidHide ready" : "HidHide not installed";
        if (_hidHide.Available && _cli.ExePath != null)
        {
            // The bridge process must keep seeing hidden devices.
            _hidHide.RegisterApplication(_cli.ExePath);
        }
        _timer.Tick += (_, _) => RefreshDevices();
        _timer.Start();
        Loaded += (_, _) => RefreshDevices();
        Closing += OnClosingCleanup;
    }

    private void OnClosingCleanup(object? sender, CancelEventArgs e)
    {
        foreach (var session in _sessions.Values)
        {
            session.Stop();
            if (session.HiddenInstanceId != null) _hidHide.Unhide(session.HiddenInstanceId);
        }
        _sessions.Clear();
    }

    private void OnRefreshClick(object sender, RoutedEventArgs e) => RefreshDevices();

    private async void RefreshDevices()
    {
        if (_busy || _cli.ExePath == null) return;
        _busy = true;
        try
        {
            var devices = await Task.Run(() => _cli.ListBt());
            // Drop dead sessions (child exited on its own).
            foreach (var (key, session) in _sessions.ToList())
            {
                if (session.HasExited)
                {
                    if (session.HiddenInstanceId != null) _hidHide.Unhide(session.HiddenInstanceId);
                    _sessions.Remove(key);
                    Log($"session ended: {key}");
                }
            }

            var seen = new HashSet<string>();
            foreach (var d in devices)
            {
                if (string.IsNullOrEmpty(d.InstanceId)) continue;
                // Same rule as the TV app: paired-but-offline devices (HID
                // interface not visible) are not listed at all. A row we hold
                // a session for stays visible regardless.
                if (!d.CanOpen && !_sessions.ContainsKey(d.InstanceId)) continue;
                seen.Add(d.InstanceId);
                if (!_byKey.TryGetValue(d.InstanceId, out var vm))
                {
                    vm = new DeviceVm();
                    _byKey[d.InstanceId] = vm;
                }
                vm.UpdateFrom(d, _sessions.ContainsKey(d.InstanceId), _hidHide.Available);
            }
            foreach (var key in _byKey.Keys.ToList())
            {
                if (!seen.Contains(key) && !_sessions.ContainsKey(key)) _byKey.Remove(key);
            }

            var ordered = _byKey.Values
                .OrderByDescending(v => v.Plugged)
                .ThenByDescending(v => v.Connected)
                .ThenBy(v => v.Product)
                .ToList();
            DeviceList.ItemsSource = ordered;
            EmptyText.Visibility = ordered.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            SubtitleText.Text = $"local Bluetooth controllers → virtual USB   ·   {ordered.Count} device{(ordered.Count == 1 ? "" : "s")}";
        }
        catch (Exception ex)
        {
            Log($"refresh failed: {ex.Message}");
        }
        finally
        {
            _busy = false;
        }
    }

    private async void OnPlugClick(object sender, RoutedEventArgs e)
    {
        if (((FrameworkElement)sender).DataContext is not DeviceVm vm || _cli.ExePath == null) return;

        if (_sessions.TryGetValue(vm.InstanceId, out var existing))
        {
            Log($"unplugging {vm.Product}…");
            await Task.Run(existing.Stop);
            if (existing.HiddenInstanceId != null) _hidHide.Unhide(existing.HiddenInstanceId);
            _sessions.Remove(vm.InstanceId);
            vm.SetPlugged(false);
            Log($"{vm.Product} unplugged");
            RefreshDevices();
            return;
        }

        if (!vm.Connected)
        {
            Log($"{vm.Product} is not connected (turn it on first)");
            return;
        }

        var slot = NextFreeSlot();
        var session = new PlugSession(_cli.ExePath, vm, slot, Log);
        Log($"plugging {vm.Product}…");
        var ok = await Task.Run(session.Start);
        if (!ok)
        {
            Log($"plug failed for {vm.Product} — see log");
            return;
        }
        if (vm.HideFromGames && _hidHide.Available)
        {
            if (_hidHide.Hide(vm.InstanceId)) session.HiddenInstanceId = vm.InstanceId;
            else Log($"HidHide hide failed for {vm.Product} (games may see double input)");
        }
        _sessions[vm.InstanceId] = session;
        vm.SetPlugged(true);
        Log($"{vm.Product} plugged (busid {session.BusId}, port {session.UsbipPort})");
        RefreshDevices();
    }

    private int NextFreeSlot()
    {
        var used = _sessions.Values.Select(s => s.Slot).ToHashSet();
        for (var i = 0; ; ++i) if (!used.Contains(i)) return i;
    }

    private void Log(string line)
    {
        if (!Dispatcher.CheckAccess()) { Dispatcher.BeginInvoke(() => Log(line)); return; }
        LogText.Text = $"{DateTime.Now:HH:mm:ss}  {line}";
    }
}

/// <summary>One row in the device list.</summary>
public class DeviceVm : INotifyPropertyChanged
{
    public string InstanceId { get; private set; } = "";
    public string Product { get; private set; } = "";
    public string TypeBadge { get; private set; } = "HID";
    public string DeviceType { get; private set; } = "";
    public int Index { get; private set; }
    public bool Connected { get; private set; }
    public bool Plugged { get; private set; }
    public bool IsGameController { get; private set; }
    public string UnavailableReason { get; private set; } = "";
    public bool HideFromGames { get; set; }
    public bool CanToggleHide => !Plugged;

    public string PlugLabel => Plugged ? "Plug out" : "Plug in";
    public bool CanPlug => Plugged || Connected;
    public double CardOpacity => Connected || Plugged ? 1.0 : 0.55;
    public Visibility HideVisibility { get; private set; } = Visibility.Visible;

    public Brush StatusBrush => Plugged
        ? new SolidColorBrush(Color.FromRgb(0x43, 0xD1, 0x7C))
        : Connected
            ? new SolidColorBrush(Color.FromRgb(0x4D, 0xA3, 0xFF))
            : new SolidColorBrush(Color.FromRgb(0x5A, 0x6B, 0x80));

    public string StatusText => Plugged
        ? "bridged as virtual USB device"
        : Connected
            ? "connected — ready to plug"
            : string.IsNullOrEmpty(UnavailableReason) ? "not connected" : $"not connected ({UnavailableReason})";

    public void UpdateFrom(CtmDevice d, bool plugged, bool hidHideAvailable)
    {
        InstanceId = d.InstanceId;
        Product = string.IsNullOrWhiteSpace(d.Product) ? "Unnamed device" : d.Product;
        DeviceType = d.DeviceType;
        Index = d.Index;
        Connected = d.CanOpen;
        IsGameController = d.IsGameController;
        UnavailableReason = d.UnavailableReason;
        TypeBadge = d.DeviceType switch
        {
            "ds4_bt" or "ds4_usb" => "DS4",
            "ds5_bt" or "ds5_usb" => "DS5",
            _ when d.UsagePage == 1 && d.Usage == 6 => "KEYBOARD",
            _ when d.UsagePage == 1 && d.Usage == 2 => "MOUSE",
            _ when d.IsGameController => "CONTROLLER",
            _ => "HID",
        };
        if (!Plugged && !plugged)
        {
            // Default: hide pads from games, never default-hide keyboards/mice
            // (hiding your only mouse hides it from everything).
            HideFromGames = d.IsGameController;
        }
        Plugged = plugged;
        HideVisibility = hidHideAvailable && IsGameController ? Visibility.Visible : Visibility.Collapsed;
        Notify();
    }

    public void SetPlugged(bool plugged) { Plugged = plugged; Notify(); }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void Notify() => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(string.Empty));
}

/// <summary>Parsed row of `ctm-usbip list-bt` JSON.</summary>
public class CtmDevice
{
    public int Index { get; set; }
    public string Product { get; set; } = "";
    public string DeviceType { get; set; } = "";
    public string UnavailableReason { get; set; } = "";
    public string InstanceId { get; set; } = "";
    public int UsagePage { get; set; }
    public int Usage { get; set; }
    public bool IsGameController { get; set; }
    public bool CanOpen { get; set; }
}

/// <summary>Locates and runs ctm-usbip.exe (list + plug sessions).</summary>
public class CtmCli
{
    public string? ExePath { get; }

    public CtmCli()
    {
        var candidates = new[]
        {
            Path.Combine(AppContext.BaseDirectory, "ctm-usbip.exe"),
            Path.Combine(AppContext.BaseDirectory, "..", "ctm-usbip.exe"),
            @"C:\Program Files\CTM Bridge\ctm-usbip.exe",
        };
        ExePath = candidates.Select(Path.GetFullPath).FirstOrDefault(File.Exists);
    }

    public List<CtmDevice> ListBt()
    {
        if (ExePath == null) return new List<CtmDevice>();
        var psi = new System.Diagnostics.ProcessStartInfo(ExePath, "list-bt")
        {
            RedirectStandardOutput = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        using var p = System.Diagnostics.Process.Start(psi)!;
        var json = p.StandardOutput.ReadToEnd();
        p.WaitForExit(10000);
        var rows = JsonSerializer.Deserialize<List<JsonElement>>(json) ?? new();
        return rows.Select(r => new CtmDevice
        {
            Index = r.GetProperty("index").GetInt32(),
            Product = r.GetProperty("product").GetString() ?? "",
            DeviceType = r.GetProperty("device_type").GetString() ?? "",
            UnavailableReason = r.GetProperty("unavailable_reason").GetString() ?? "",
            InstanceId = r.GetProperty("instance_id").GetString() ?? "",
            UsagePage = r.GetProperty("usage_page").GetInt32(),
            Usage = r.GetProperty("usage").GetInt32(),
            IsGameController = r.GetProperty("is_game_controller").GetBoolean(),
            CanOpen = r.GetProperty("can_open").GetBoolean(),
        }).ToList();
    }
}
