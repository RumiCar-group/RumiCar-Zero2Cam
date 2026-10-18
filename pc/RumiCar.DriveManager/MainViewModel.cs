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
    public string Display =>
        $"{(Kind == "logic" ? "ロジック" : "データ")}  {Name}  {(IsCurrent ? "[使用中]" : IsPrevious ? "[直前]" : "")}{(Verified ? "" : "  !検証失敗: " + Error)}";
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
    private string _battery = "-";
    private string _frames = "0";
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
    public string Battery { get => _battery; set => Set(ref _battery, value); }
    public string Frames { get => _frames; set => Set(ref _frames, value); }
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
