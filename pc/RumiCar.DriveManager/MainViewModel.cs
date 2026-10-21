using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;   // WPF projects do not get System.IO from implicit usings (System.Windows.Shapes.Path clash)
using System.Runtime.CompilerServices;
using System.Windows.Media.Imaging;
using RumiCar.Protocol;

namespace RumiCar.DriveManager;

/// <summary>One installed logic or pack on the car (from list_versions).</summary>
public sealed class VersionRow
{
    public string Kind { get; init; } = "";       // "logic" or "pack"
    public string Name { get; init; } = "";       // directory name
    public string Version { get; init; } = "";
    public bool Verified { get; init; }
    public string Error { get; init; } = "";
    public bool IsCurrent { get; init; }
    public bool IsPrevious { get; init; }
    public string Description { get; init; } = "";
    public IReadOnlyList<string> Capabilities { get; init; } = Array.Empty<string>();
    public IReadOnlyList<string> Scenarios { get; init; } = Array.Empty<string>();
    public string Parent { get; init; } = "";
    public string Display =>
        $"{(Kind == "logic" ? "ロジック" : "データ")}  {Name}  {(IsCurrent ? "[使用中]" : IsPrevious ? "[直前]" : "")}"
        + (Verified ? "" : "  !検証失敗: " + Error)
        + (Capabilities.Count > 0 ? "  [" + Capability.Describe(Capabilities) + "]" : "")
        + (Description.Length > 0 ? "  " + Description : "");
    /// <summary>Everything known about the version, for the detail line under the list.</summary>
    public string Details
    {
        get
        {
            var parts = new List<string> { $"{(Kind == "logic" ? "ロジック" : "データ")} {Name}" };
            if (Description.Length > 0) parts.Add(Description);
            if (Capabilities.Count > 0) parts.Add("対応: " + Capability.Describe(Capabilities));
            if (Scenarios.Count > 0) parts.Add("学習元: " + string.Join(", ", Scenarios));
            if (Parent.Length > 0) parts.Add("元の版: " + Parent);
            if (!Verified) parts.Add("検証失敗: " + Error);
            return string.Join("\n", parts);
        }
    }
}

/// <summary>Capability names carried in pack manifests (docs 4.1), shown in Japanese.</summary>
public static class Capability
{
    public static string Describe(IEnumerable<string> names) => string.Join(" / ", names.Select(Describe));

    public static string Describe(string name) => name switch
    {
        "walls" => "壁",
        "traffic_light" => "信号機",
        "vehicle" => "車両",
        "intersection" => "交差点",
        _ => name,
    };

    /// <summary>Detector names of the cam_drive logic, shown in Japanese.</summary>
    public static string DescribeDetector(string name) => name switch
    {
        "traffic_light_red" => "赤信号",
        "traffic_light_green" => "青信号",
        "vehicle" => "車両",
        "intersection" => "交差点",
        "intersection_left" => "交差点(左)",
        "intersection_right" => "交差点(右)",
        "" => "?",
        _ => name,
    };

    /// <summary>State names of the shipped logics (tof_drive, cam_drive), shown in Japanese next to the name.</summary>
    public static string DescribeState(string name) => name switch
    {
        "CRUISE" => "巡航",
        "TURN_LEFT" => "左へ",
        "TURN_RIGHT" => "右へ",
        "EMERGENCY_BRAKE" => "非常ブレーキ",
        "RECOVER_REVERSE" => "後退して立て直し",
        "SENSOR_FAULT" => "センサー異常",
        "SIGNAL_APPROACH" => "赤信号に接近",
        "SIGNAL_STOP" => "赤信号で停止",
        "SIGNAL_RESUME" => "信号から発進",
        "FOLLOW" => "前の車両に追従",
        "FOLLOW_WAIT" => "駐車車両の後ろで待機",
        "OVERTAKE_OUT" => "追い越し：はみ出す",
        "OVERTAKE_PASS" => "追い越し：横を通る",
        "OVERTAKE_IN" => "追い越し：戻る",
        "INTERSECTION_APPROACH" => "交差点に接近",
        "INTERSECTION_STOP" => "交差点で停止",
        "INTERSECTION_CROSS" => "交差点を直進",
        "INTERSECTION_TURN" => "交差点で曲がる",
        _ => "",
    };
}

/// <summary>Firmware throttling flags (telemetry "throttled"), shown in Japanese (docs 3.9).</summary>
public static class PowerFlags
{
    public const uint UnderVoltageNow = 0x1;
    public const uint UnderVoltageSinceBoot = 0x10000;
    public const uint ThrottledNow = 0x4;
    public const uint SoftTemperatureLimit = 0x8;

    public static string Describe(uint flags)
    {
        if (flags == 0) return "正常（低電圧なし）";
        var parts = new List<string>();
        if ((flags & UnderVoltageNow) != 0) parts.Add("低電圧（現在）");
        else if ((flags & UnderVoltageSinceBoot) != 0) parts.Add("低電圧あり（起動後）");
        if ((flags & ThrottledNow) != 0) parts.Add("処理速度制限中");
        if ((flags & SoftTemperatureLimit) != 0) parts.Add("温度制限中");
        if (parts.Count == 0) parts.Add($"フラグ 0x{flags:X}");
        return string.Join("、", parts);
    }
}

/// <summary>State shown by the main window.  Updated on the UI thread only.</summary>
public sealed class MainViewModel : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    private string _host = CarDefaults.Host;
    private string _carName = CarDefaults.Ssid;
    private string _dataRoot = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "RumiCarData");
    private string _connectionState = "切断";
    private bool _connected;
    private bool _recording;
    private string _mode = "-";
    private string _logicState = "-";
    private string _tof = "- / - / -";
    private string _command = "-";
    private string _logicCommand = "-";
    private string _estop = "-";
    private string _versions = "-";
    private string _packInfo = "-";
    private string _packCapabilities = "-";
    private string _packScenarios = "-";
    private string _power = "-";
    private string _startupMode = "-";
    private string _scenario = "";
    private string _note = "";
    private string _videoStatus = "-";
    private string _battery = "-";
    private string _frames = "0";
    private string _images = "0";
    private bool _showImage = true;
    private bool _showOverlay = true;
    private string _detections = "-";
    private bool _autoRecord = true;
    private string _recordingStatus = "停止（接続すると自動で始まります）";
    private string _pushStatus = "-";
    private VersionRow? _selectedVersion;
    private BitmapImage? _image;

    public string Host { get => _host; set => Set(ref _host, value); }
    public string CarName { get => _carName; set => Set(ref _carName, value); }
    public string DataRoot { get => _dataRoot; set => Set(ref _dataRoot, value); }
    public string ConnectionState { get => _connectionState; set => Set(ref _connectionState, value); }
    public bool Connected { get => _connected; set { if (Set(ref _connected, value)) OnPropertyChanged(nameof(NotConnected)); } }
    public bool NotConnected => !_connected;
    public bool Recording { get => _recording; set => Set(ref _recording, value); }
    public string Mode { get => _mode; set => Set(ref _mode, value); }
    public string LogicState { get => _logicState; set => Set(ref _logicState, value); }
    public string Tof { get => _tof; set => Set(ref _tof, value); }
    public string Command { get => _command; set => Set(ref _command, value); }
    public string LogicCommand { get => _logicCommand; set => Set(ref _logicCommand, value); }
    public string EStop { get => _estop; set => Set(ref _estop, value); }
    public string Versions { get => _versions; set => Set(ref _versions, value); }
    /// <summary>Active pack: id, version and its description from the manifest.</summary>
    public string PackInfo { get => _packInfo; set => Set(ref _packInfo, value); }
    public string PackCapabilities { get => _packCapabilities; set => Set(ref _packCapabilities, value); }
    public string PackScenarios { get => _packScenarios; set => Set(ref _packScenarios, value); }
    /// <summary>Under-voltage / throttling state reported by the car's firmware.</summary>
    public string Power { get => _power; set => Set(ref _power, value); }
    public string StartupMode { get => _startupMode; set => Set(ref _startupMode, value); }
    /// <summary>Scenario name written into the next recording's session.json.</summary>
    public string Scenario { get => _scenario; set => Set(ref _scenario, value); }
    public string Note { get => _note; set => Set(ref _note, value); }
    public string VideoStatus { get => _videoStatus; set => Set(ref _videoStatus, value); }
    public ObservableCollection<string> ScenarioHistory { get; } = new();
    public string Battery { get => _battery; set => Set(ref _battery, value); }
    public string Frames { get => _frames; set => Set(ref _frames, value); }
    /// <summary>Frames that carried a camera image.</summary>
    public string Images { get => _images; set => Set(ref _images, value); }
    /// <summary>Show the live image.  Off: frames are still received and recorded, just not decoded or drawn.</summary>
    public bool ShowImage
    {
        get => _showImage;
        set { if (Set(ref _showImage, value)) { OnPropertyChanged(nameof(ImageVisibility)); OnPropertyChanged(nameof(ImageHiddenNoteVisibility)); if (!value) Image = null; } }
    }
    public System.Windows.Visibility ImageVisibility => _showImage ? System.Windows.Visibility.Visible : System.Windows.Visibility.Collapsed;
    /// <summary>Draw the logic's detections (aux output) over the image.</summary>
    public bool ShowOverlay { get => _showOverlay; set => Set(ref _showOverlay, value); }
    /// <summary>Text form of the detections for the status card.</summary>
    public string Detections { get => _detections; set => Set(ref _detections, value); }
    public System.Windows.Visibility ImageHiddenNoteVisibility => _showImage ? System.Windows.Visibility.Collapsed : System.Windows.Visibility.Visible;
    /// <summary>Start a recording session whenever a connection is made (default on).</summary>
    public bool AutoRecord
    {
        get => _autoRecord;
        set { if (Set(ref _autoRecord, value) && !_recording) RecordingStatus = value ? "停止（接続すると自動で始まります）" : "停止"; }
    }
    public string RecordingStatus { get => _recordingStatus; set => Set(ref _recordingStatus, value); }
    public string PushStatus { get => _pushStatus; set => Set(ref _pushStatus, value); }
    public VersionRow? SelectedVersion { get => _selectedVersion; set => Set(ref _selectedVersion, value); }
    public ObservableCollection<VersionRow> InstalledVersions { get; } = new();
    public BitmapImage? Image { get => _image; set => Set(ref _image, value); }

    public ObservableCollection<string> Log { get; } = new();

    public void AddLog(string line)
    {
        Log.Add($"{DateTime.Now:HH:mm:ss} {line}");
        while (Log.Count > 500) Log.RemoveAt(0);
    }

    private bool Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        OnPropertyChanged(name);
        return true;
    }

    private void OnPropertyChanged(string? name) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
