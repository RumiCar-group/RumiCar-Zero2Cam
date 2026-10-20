using System.ComponentModel;
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using RumiCar.Protocol.Control;
using RumiCar.Protocol.Packs;
using RumiCar.Protocol.Recording;
using RumiCar.Protocol.Telemetry;

namespace RumiCar.DriveManager;

public partial class MainWindow : Window
{
    private readonly MainViewModel _vm = new();
    private CarConnection? _car;
    private bool _closing;

    // Recording runs on the telemetry thread so that every frame is written
    // even when the UI is busy; the lock covers start/stop from the UI thread.
    private readonly object _sessionLock = new();
    private SessionWriter? _session;

    // Display shows the newest frame only; intermediate frames are dropped
    // from the screen (never from the recording) when the UI falls behind.
    private readonly object _frameLock = new();
    private TelemetryFrame? _latestFrame;
    private bool _renderQueued;
    private int _framesReceived;
    private int _imagesReceived;

    private int _manualSteer;
    private int _manualDrive;
    private bool _manualMode;
    private bool _pushing;

    public MainWindow()
    {
        InitializeComponent();
        DataContext = _vm;
        PreviewKeyDown += OnKey;
        PreviewKeyUp += OnKey;
    }

    // --- connection ----------------------------------------------------------

    private async void OnConnectClick(object sender, RoutedEventArgs e)
    {
        if (_car is not null)
        {
            await DisconnectAsync();
            return;
        }
        var car = new CarConnection(_vm.Host.Trim());
        car.Log += line => Ui(() => _vm.AddLog(line));
        car.EventReceived += evt => Ui(() => OnCarEvent(evt));
        car.FrameReceived += OnFrameReceived;
        car.Disconnected += ex => Ui(() => OnDisconnected(car, ex));
        _vm.ConnectionState = "接続中…";
        ConnectButton.IsEnabled = false;
        try
        {
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            await car.ConnectAsync("RumiCarDriveManager", cts.Token);
            _car = car;
            _framesReceived = 0;
            _imagesReceived = 0;
            _vm.Frames = "0";
            _vm.Images = "0";
            _vm.Connected = true;
            _vm.ConnectionState = "接続";
            ConnectButton.Content = "切断";
            var carName = CarConnection.GetString(car.HelloResult, "car");
            var coreVersion = CarConnection.GetString(car.HelloResult, "core_version");
            if (carName is not null) _vm.AddLog($"車両 {carName}, 本体 {coreVersion ?? "?"}");
            var status = await car.RequestAsync(ControlCodec.GetStatus(car.NextId()), CancellationToken.None);
            if (status.Ok && status.Result is JsonElement result) ApplyStatus(result);
            else if (!status.Ok) _vm.AddLog("状態取得: 拒否 (" + status.Error + ")");
            await RefreshVersionsAsync();
        }
        catch (Exception ex)
        {
            _vm.AddLog("接続できません: " + ex.Message);
            _vm.ConnectionState = "切断";
            await car.DisposeAsync();
        }
        finally
        {
            ConnectButton.IsEnabled = true;
        }
    }

    private async Task DisconnectAsync()
    {
        StopRecording();
        var car = _car;
        _car = null;
        _vm.Connected = false;
        _vm.ConnectionState = "切断";
        _manualMode = false;
        _manualSteer = 0;
        _manualDrive = 0;
        ConnectButton.Content = "接続";
        _vm.InstalledVersions.Clear();
        _vm.PushStatus = "-";
        if (car is not null) await car.DisposeAsync();
    }

    private void OnDisconnected(CarConnection source, Exception? ex)
    {
        if (!ReferenceEquals(source, _car)) return;   // already disconnected by the user
        _vm.AddLog(ex is null ? "車両との接続が閉じられました" : "接続が切れました: " + ex.Message);
        _ = DisconnectAsync();
    }

    protected override async void OnClosing(CancelEventArgs e)
    {
        base.OnClosing(e);
        if (e.Cancel) return;
        if (_car is null && _session is null) return;   // nothing to shut down: close proceeds
        e.Cancel = true;                                 // finish the disconnect first, then close again
        if (_closing) return;
        _closing = true;
        await DisconnectAsync();
        Close();
    }

    /// <summary>Runs an update on the UI thread; dropped once the window is shutting down.</summary>
    private void Ui(Action action)
    {
        if (Dispatcher.HasShutdownStarted) return;
        _ = Dispatcher.InvokeAsync(action, DispatcherPriority.Normal);
    }

    // --- incoming data --------------------------------------------------------

    private void OnCarEvent(ControlEvent evt)
    {
        switch (evt.Event)
        {
            case "status":
                if (evt.Data is JsonElement data) ApplyStatus(data);
                break;
            case "estop":
            {
                var active = evt.Data is JsonElement d && d.ValueKind == JsonValueKind.Object && d.TryGetProperty("active", out var a) && a.ValueKind == JsonValueKind.True;
                var reason = CarConnection.GetString(evt.Data, "reason") ?? "";
                _vm.EStop = FormatEStop(active, reason);
                _vm.AddLog(active ? "非常停止 作動: " + DescribeEStopReason(reason) : "非常停止 解除");
                break;
            }
            case "activation":
            {
                var ok = evt.Data is JsonElement d && d.ValueKind == JsonValueKind.Object && d.TryGetProperty("ok", out var o) && o.ValueKind == JsonValueKind.True;
                var error = CarConnection.GetString(evt.Data, "error") ?? "";
                _vm.AddLog(ok ? "切り替え完了: " + (CarConnection.GetString(evt.Data, "logic_dir") ?? "") + " / " + (CarConnection.GetString(evt.Data, "pack_dir") ?? "")
                              : "切り替え失敗（現行版を継続）: " + error);
                _ = RefreshVersionsAsync();
                break;
            }
            default:
                _vm.AddLog("event " + evt.Event);
                break;
        }
    }

    private void ApplyStatus(JsonElement data)
    {
        var mode = CarConnection.GetString(data, "mode");
        if (mode is not null) SetMode(mode);
        var lv = CarConnection.GetString(data, "logic_version");
        var pv = CarConnection.GetString(data, "pack_version");
        if (lv is not null || pv is not null) _vm.Versions = $"{lv ?? "?"} / {pv ?? "?"}";
        if (data.ValueKind == JsonValueKind.Object && data.TryGetProperty("battery_mv", out var mv) && mv.TryGetInt32(out var millivolts))
            _vm.Battery = FormatBattery(millivolts);
        if (data.ValueKind == JsonValueKind.Object && data.TryGetProperty("estop", out var es) && (es.ValueKind == JsonValueKind.True || es.ValueKind == JsonValueKind.False))
            _vm.EStop = FormatEStop(es.ValueKind == JsonValueKind.True, CarConnection.GetString(data, "estop_reason") ?? "");
        var stateName = CarConnection.GetString(data, "state_name");
        if (stateName is not null) _vm.LogicState = stateName;
    }

    private static string FormatEStop(bool active, string reason) => active ? "作動中（" + DescribeEStopReason(reason) + "）" : "なし";

    private static string DescribeEStopReason(string reason) => reason switch
    {
        "request" => "PC からの要求",
        "heartbeat" => "heartbeat 途絶",
        "disconnected" => "手動モード中に切断",
        "" => "-",
        _ => reason,
    };

    /// <summary>Steering plus drive as direction and PWM duty (per mille shown as %).</summary>
    private static string DescribeCommand(int steer, int drivePermille, bool brake)
    {
        var steering = steer < 0 ? "左" : steer > 0 ? "右" : "中央";
        var drive = brake ? "ブレーキ"
                  : drivePermille > 0 ? $"前進 {drivePermille / 10.0:F1}%"
                  : drivePermille < 0 ? $"後退 {-drivePermille / 10.0:F1}%"
                  : "停止";
        return $"操舵 {steering} / {drive}";
    }

    private void SetMode(string mode)
    {
        _vm.Mode = mode;
        _manualMode = mode == DriveMode.Manual;
        if (!_manualMode)
        {
            _manualSteer = 0;
            _manualDrive = 0;
        }
    }

    // Telemetry thread.
    private void OnFrameReceived(TelemetryFrame frame)
    {
        Interlocked.Increment(ref _framesReceived);
        if (frame.Jpeg.Length > 0) Interlocked.Increment(ref _imagesReceived);
        try
        {
            lock (_sessionLock) _session?.Write(frame);
        }
        catch (Exception ex)
        {
            Ui(() =>
            {
                _vm.AddLog("収録エラー、収録を停止します: " + ex.Message);
                StopRecording();
            });
        }

        bool queue;
        lock (_frameLock)
        {
            _latestFrame = frame;
            queue = !_renderQueued;
            _renderQueued = true;
        }
        if (queue) Ui(RenderLatestFrame);
    }

    // UI thread.
    private void RenderLatestFrame()
    {
        TelemetryFrame? frame;
        lock (_frameLock)
        {
            frame = _latestFrame;
            _latestFrame = null;
            _renderQueued = false;
        }
        if (frame is null) return;

        _vm.Frames = Volatile.Read(ref _framesReceived).ToString();
        _vm.Images = Volatile.Read(ref _imagesReceived).ToString();
        var h = frame.Header;
        _vm.Tof = $"{FormatTof(h.TofMm[0])} / {FormatTof(h.TofMm[1])} / {FormatTof(h.TofMm[2])}";
        _vm.LogicState = h.StateName.Length > 0 ? h.StateName : h.State.ToString();
        _vm.Command = DescribeCommand(h.Steer, h.DrivePermille, h.Brake);
        _vm.LogicCommand = DescribeCommand(h.LogicSteer, h.LogicDrivePermille, (h.Flags & 0x3) != 0);   // BRAKE or FAULT flag
        if (h.Mode.Length > 0 && h.Mode != _vm.Mode) SetMode(h.Mode);
        var estopText = h.EStop ? "作動中" : "なし";
        if (!_vm.EStop.StartsWith(estopText, StringComparison.Ordinal)) _vm.EStop = estopText;
        if (h.LogicVersion.Length > 0 || h.PackVersion.Length > 0) _vm.Versions = $"{h.LogicVersion} / {h.PackVersion}";
        if (h.BatteryMillivolts > 0) _vm.Battery = FormatBattery(h.BatteryMillivolts);

        if (frame.Jpeg.Length == 0 || !_vm.ShowImage) return;   // hidden: no decoding either
        try
        {
            var bmp = new BitmapImage();
            using var ms = new MemoryStream(frame.Jpeg);
            bmp.BeginInit();
            bmp.CacheOption = BitmapCacheOption.OnLoad;
            bmp.StreamSource = ms;
            bmp.EndInit();
            bmp.Freeze();
            _vm.Image = bmp;
        }
        catch (Exception ex)
        {
            _vm.AddLog("画像を表示できません: " + ex.Message);
        }
    }

    private static string FormatTof(int mm) => mm < 0 ? "--" : mm >= 8190 ? "open" : mm.ToString();

    private static string FormatBattery(int millivolts) => $"{millivolts / 1000.0:F2} V";

    // --- commands ---------------------------------------------------------------

    private async Task SendAsync(Func<CarConnection, ControlRequest> build, string description)
    {
        var car = _car;
        if (car is null) return;
        try
        {
            var r = await car.RequestAsync(build(car), CancellationToken.None);
            _vm.AddLog(r.Ok ? $"{description}: OK" : $"{description}: 拒否 ({r.Error})");
        }
        catch (Exception ex)
        {
            _vm.AddLog($"{description}: 失敗 ({ex.Message})");
        }
    }

    private async void OnEStopClick(object sender, RoutedEventArgs e) =>
        await SendAsync(car => ControlCodec.EStop(car.NextId()), "非常停止");

    private async void OnEStopClearClick(object sender, RoutedEventArgs e) =>
        await SendAsync(car => ControlCodec.EStopClear(car.NextId()), "非常停止解除");

    private async void OnModeAutoClick(object sender, RoutedEventArgs e) => await SetModeAsync(DriveMode.Auto);
    private async void OnModeManualClick(object sender, RoutedEventArgs e) => await SetModeAsync(DriveMode.Manual);
    private async void OnModeRecordOnlyClick(object sender, RoutedEventArgs e) => await SetModeAsync(DriveMode.RecordOnly);

    private Task SetModeAsync(string mode) =>
        SendAsync(car => ControlCodec.SetMode(car.NextId(), mode), "モード " + mode);

    // Clicking the image takes keyboard focus away from the text boxes so the arrow keys drive.
    private void OnImageClick(object sender, MouseButtonEventArgs e) => ImageBorder.Focus();

    // Manual driving with the arrow keys; only effective while the car is in manual mode.
    private async void OnKey(object sender, KeyEventArgs e)
    {
        var car = _car;
        if (car is null || !_manualMode) return;
        if (e.OriginalSource is System.Windows.Controls.TextBox) return;
        var down = e.IsDown;
        int steer = _manualSteer, drive = _manualDrive;
        switch (e.Key)
        {
            case Key.Left: steer = down ? -1 : (steer == -1 ? 0 : steer); break;
            case Key.Right: steer = down ? 1 : (steer == 1 ? 0 : steer); break;
            case Key.Up: drive = down ? 500 : (drive > 0 ? 0 : drive); break;
            case Key.Down: drive = down ? -500 : (drive < 0 ? 0 : drive); break;
            default: return;
        }
        e.Handled = true;
        if (steer == _manualSteer && drive == _manualDrive) return;   // key repeat
        _manualSteer = steer;
        _manualDrive = drive;
        try
        {
            var r = await car.RequestAsync(ControlCodec.ManualDrive(car.NextId(), steer, drive), CancellationToken.None);
            if (!r.Ok) _vm.AddLog("手動操作: 拒否 (" + r.Error + ")");
        }
        catch (Exception ex)
        {
            _vm.AddLog("手動操作を送れません: " + ex.Message);
        }
    }

    // --- versions (list / push / activate / rollback / delete) ------------------

    private async Task RefreshVersionsAsync()
    {
        var car = _car;
        if (car is null) return;
        try
        {
            var r = await car.RequestAsync(ControlCodec.ListVersions(car.NextId()), CancellationToken.None);
            if (!r.Ok || r.Result is not JsonElement result)
            {
                _vm.AddLog("版の一覧: 拒否 (" + r.Error + ")");
                return;
            }
            var selected = _vm.SelectedVersion?.Name;
            _vm.InstalledVersions.Clear();
            foreach (var (kind, key) in new[] { ("logic", "logic"), ("pack", "packs") })
            {
                var current = GetNested(result, "current", kind);
                var previous = GetNested(result, "previous", kind);
                if (!result.TryGetProperty(key, out var arr) || arr.ValueKind != JsonValueKind.Array) continue;
                foreach (var v in arr.EnumerateArray())
                {
                    var name = CarConnection.GetString(v, "name") ?? "";
                    _vm.InstalledVersions.Add(new VersionRow
                    {
                        Kind = kind,
                        Name = name,
                        Version = CarConnection.GetString(v, "version") ?? "",
                        Verified = v.TryGetProperty("verified", out var ok) && ok.ValueKind == JsonValueKind.True,
                        Error = CarConnection.GetString(v, "error") ?? "",
                        IsCurrent = name == current,
                        IsPrevious = name == previous,
                    });
                }
            }
            _vm.SelectedVersion = _vm.InstalledVersions.FirstOrDefault(x => x.Name == selected);
        }
        catch (Exception ex)
        {
            _vm.AddLog("版の一覧: 失敗 (" + ex.Message + ")");
        }
    }

    private static string? GetNested(JsonElement e, string outer, string inner) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(outer, out var o) ? CarConnection.GetString(o, inner) : null;

    private async void OnVersionsRefreshClick(object sender, RoutedEventArgs e) => await RefreshVersionsAsync();

    private async void OnPushClick(object sender, RoutedEventArgs e)
    {
        var car = _car;
        if (car is null || _pushing) return;
        var dialog = new Microsoft.Win32.OpenFolderDialog { Title = "車に送るロジック／データのフォルダ（manifest.json を含む）" };
        if (dialog.ShowDialog(this) != true) return;
        var directory = dialog.FolderName;
        VersionPush.Plan plan;
        try
        {
            plan = VersionPush.Prepare(directory);
        }
        catch (Exception ex)
        {
            _vm.AddLog("送信前の検証に失敗: " + ex.Message);
            _vm.PushStatus = "検証失敗: " + ex.Message;
            return;
        }
        _pushing = true;
        _vm.PushStatus = $"送信中 {plan.Kind} {plan.DirectoryName} (0 / {plan.TotalBytes} バイト)";
        _vm.AddLog($"送信開始: {plan.Kind} {plan.DirectoryName} ({plan.Files.Count} ファイル, {plan.TotalBytes} バイト)");
        var progress = new Progress<long>(sent => _vm.PushStatus = $"送信中 {plan.Kind} {plan.DirectoryName} ({sent} / {plan.TotalBytes} バイト)");
        try
        {
            await VersionPush.PushAsync(directory, plan, car.RequestAsync, car.NextId, progress, CancellationToken.None);
            _vm.PushStatus = $"送信完了: {plan.Kind} {plan.DirectoryName}（「有効化」で切り替え）";
            _vm.AddLog("送信完了: " + plan.DirectoryName);
            await RefreshVersionsAsync();
            _vm.SelectedVersion = _vm.InstalledVersions.FirstOrDefault(x => x.Name == plan.DirectoryName);
        }
        catch (Exception ex)
        {
            _vm.PushStatus = "送信失敗: " + ex.Message;
            _vm.AddLog("送信失敗: " + ex.Message);
        }
        finally
        {
            _pushing = false;
        }
    }

    private async void OnActivateClick(object sender, RoutedEventArgs e)
    {
        var v = _vm.SelectedVersion;
        if (v is null) { _vm.AddLog("有効化: 一覧から版を選んでください"); return; }
        if (v.IsCurrent) { _vm.AddLog("有効化: " + v.Name + " は使用中です"); return; }
        await SendAsync(car => ControlCodec.Activate(car.NextId(), v.Kind, v.Name), "有効化 " + v.Name);
        await RefreshVersionsAsync();
    }

    private async void OnRollbackLogicClick(object sender, RoutedEventArgs e)
    {
        await SendAsync(car => ControlCodec.Rollback(car.NextId(), "logic"), "ロジックを戻す");
        await RefreshVersionsAsync();
    }

    private async void OnRollbackPackClick(object sender, RoutedEventArgs e)
    {
        await SendAsync(car => ControlCodec.Rollback(car.NextId(), "pack"), "データを戻す");
        await RefreshVersionsAsync();
    }

    private async void OnDeleteClick(object sender, RoutedEventArgs e)
    {
        var v = _vm.SelectedVersion;
        if (v is null) { _vm.AddLog("削除: 一覧から版を選んでください"); return; }
        if (v.IsCurrent || v.IsPrevious) { _vm.AddLog("削除: 使用中または直前の版は削除できません"); return; }
        if (MessageBox.Show(this, $"{v.Kind} {v.Name} を車から削除します。よろしいですか？", "削除の確認", MessageBoxButton.OKCancel, MessageBoxImage.Warning) != MessageBoxResult.OK) return;
        await SendAsync(car => ControlCodec.DeleteVersion(car.NextId(), v.Kind, v.Name), "削除 " + v.Name);
        await RefreshVersionsAsync();
    }

    // --- recording ------------------------------------------------------------

    private void OnRecordClick(object sender, RoutedEventArgs e)
    {
        if (_session is not null)
        {
            StopRecording();
            return;
        }
        try
        {
            var session = new SessionWriter(_vm.DataRoot.Trim(), _vm.CarName.Trim(), DateTimeOffset.Now);
            lock (_sessionLock) _session = session;
            _vm.Recording = true;
            RecordButton.Content = "収録停止";
            _vm.AddLog("収録開始: " + session.Directory);
        }
        catch (Exception ex)
        {
            _vm.AddLog("収録を開始できません: " + ex.Message);
        }
    }

    private void StopRecording()
    {
        SessionWriter? s;
        lock (_sessionLock)
        {
            s = _session;
            _session = null;
        }
        if (s is null) return;
        try
        {
            s.Dispose();
        }
        catch (Exception ex)
        {
            _vm.AddLog("収録の終了処理に失敗: " + ex.Message);
        }
        _vm.Recording = false;
        RecordButton.Content = "収録開始";
        _vm.AddLog($"収録停止: {s.FramesWritten} コマ, 欠け {s.SequenceGaps} 回");
    }
}
