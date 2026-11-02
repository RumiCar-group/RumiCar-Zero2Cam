using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using RumiCar.Protocol.Releases;

namespace RumiCar.DriveManager;

/// <summary>
/// "SD カードを作る…" (docs 6.5): lists the Releases, downloads the chosen
/// image to the data folder, checks it against SHA256SUMS.txt and the
/// Release's Imager manifest, writes a local manifest that points Imager at the
/// downloaded file, and opens Raspberry Pi Imager with it.  Imager does the
/// writing.  Only Releases that carry the manifest (0.4.1 and later) can be
/// written this way; older ones are listed for information.
/// </summary>
public partial class SdCardWindow : Window
{
    private readonly ReleaseDownloader _downloader;
    private readonly string _appVersion;
    private readonly string? _carVersion;
    private readonly string _imagesFolder;
    private readonly Action<string> _log;
    private IReadOnlyList<ReleaseInfo>? _releases;
    private CancellationTokenSource? _cts;

    /// <param name="releases">The list fetched earlier by the main window (null = fetch now).</param>
    public SdCardWindow(Window owner, ReleaseDownloader downloader, string appVersion, string? carVersion, string dataRoot,
                        IReadOnlyList<ReleaseInfo>? releases, Action<string> log)
    {
        InitializeComponent();
        Owner = owner;
        _downloader = downloader;
        _appVersion = appVersion;
        _carVersion = carVersion;
        _imagesFolder = Path.Combine(dataRoot, "images");
        _log = log;
        FolderText.Text = _imagesFolder;
        ManifestUrlText.Text = ReleaseCatalog.LatestManifestUrl;
        var imager = ImagerLauncher.Find();
        ImagerText.Text = imager is not null
            ? "Raspberry Pi Imager: " + imager
            : "Raspberry Pi Imager が見つかりません。" + ImagerLauncher.DownloadPage + " から導入してください（Imager 2.0 以降）。導入後にこの画面を開き直してください";
        UpdateVersionsText(null);
        if (releases is not null) ShowReleases(releases);
        Loaded += async (_, _) => { if (_releases is null) await FetchAsync(); };
    }

    private void UpdateVersionsText(ReleaseInfo? latest)
    {
        var car = _carVersion ?? "未接続";
        var text = $"このアプリ {_appVersion}　／　車の本体 {car}";
        if (latest is not null)
        {
            text += $"　／　最新の Release {latest.Tag}";
            if (_carVersion is not null && ReleaseCatalog.CompareVersions(_carVersion, latest.Version) < 0)
                text += "（車の本体より新しい版があります）";
            else if (_carVersion is not null && ReleaseCatalog.CompareVersions(_carVersion, latest.Version) == 0)
                text += "（車の本体は最新です）";
        }
        VersionsText.Text = text;
    }

    private void ShowReleases(IReadOnlyList<ReleaseInfo> releases)
    {
        _releases = releases;
        ReleaseList.ItemsSource = releases;
        var latest = ReleaseCatalog.Latest(releases);
        ReleaseList.SelectedItem = latest ?? releases.FirstOrDefault();
        UpdateVersionsText(latest);
        FetchStatus.Text = releases.Count == 0 ? "Release がありません" : $"{releases.Count} 件（新しい順）";
        OnReleaseSelected(this, null);
    }

    private async void OnFetchClick(object sender, RoutedEventArgs e) => await FetchAsync();

    private async Task FetchAsync()
    {
        FetchButton.IsEnabled = false;
        FetchStatus.Text = "取得中…";
        try
        {
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(20));
            var releases = await _downloader.FetchReleasesAsync(cts.Token);
            ShowReleases(releases);
        }
        catch (Exception ex)
        {
            FetchStatus.Text = "取得できません: " + ex.Message + "（PC がインターネットに接続しているか確認してください）";
        }
        finally
        {
            FetchButton.IsEnabled = true;
        }
    }

    private ReleaseInfo? Selected => ReleaseList.SelectedItem as ReleaseInfo;

    private void OnReleaseSelected(object sender, SelectionChangedEventArgs? e)
    {
        var r = Selected;
        var busy = _cts is not null;
        CreateButton.IsEnabled = !busy && r is { SupportsImager: true };
        ImagerButton.IsEnabled = !busy && r is { SupportsImager: true } && r.Image is not null
                                 && File.Exists(Path.Combine(_imagesFolder, r.Image.Name))
                                 && File.Exists(LocalManifestPath(r));
        if (r is null) CreateStatus.Text = "";
        else if (!r.SupportsImager)
            CreateStatus.Text = $"{r.Tag} は Imager の一覧（rumicar-zero2cam.rpi-imager-manifest）が無い版です。{ReleaseCatalog.ReleasesPageUrl} からイメージをダウンロードし、Imager の「Use custom」で書き込んでください（README 2 章）";
        else if (ImagerButton.IsEnabled)
            CreateStatus.Text = $"{r.Image!.Name} はダウンロード済みです（「Imager を開く」で再ダウンロードせずに書き込めます）";
        else
            CreateStatus.Text = $"{r.Tag}：{r.Image!.Name}（{r.Image.Size / (1024.0 * 1024.0):F0} MB）をダウンロードします";
    }

    private string LocalManifestPath(ReleaseInfo r) => Path.Combine(_imagesFolder, $"rumicar-zero2cam-{r.Version}{ImagerManifest.Extension}");

    private async void OnCreateClick(object sender, RoutedEventArgs e)
    {
        var r = Selected;
        if (r is null || !r.SupportsImager) return;
        await RunAsync(r, download: true);
    }

    private async void OnImagerOnlyClick(object sender, RoutedEventArgs e)
    {
        var r = Selected;
        if (r is null || !r.SupportsImager) return;
        await RunAsync(r, download: false);
    }

    private void OnCancelClick(object sender, RoutedEventArgs e) => _cts?.Cancel();

    private void OnClosing(object? sender, System.ComponentModel.CancelEventArgs e) => _cts?.Cancel();

    private async Task RunAsync(ReleaseInfo r, bool download)
    {
        if (_cts is not null) return;
        var cts = new CancellationTokenSource();
        _cts = cts;
        var ct = cts.Token;
        CreateButton.IsEnabled = false;
        ImagerButton.IsEnabled = false;
        FetchButton.IsEnabled = false;
        CancelButton.IsEnabled = true;
        DownloadBar.Visibility = Visibility.Visible;
        DownloadBar.Value = 0;
        try
        {
            Directory.CreateDirectory(_imagesFolder);
            var imagePath = Path.Combine(_imagesFolder, r.Image!.Name);
            var manifestPath = LocalManifestPath(r);

            // The Release's own description of the image: what Imager will list and verify against.
            Status("Release の一覧とチェックサムを取得中…");
            var manifestJson = await _downloader.FetchTextAsync(r.Manifest!.DownloadUrl, ct);
            var entry = ImagerManifest.Read(manifestJson);
            if (Path.GetFileName(entry.Url) != r.Image.Name)
                throw new InvalidDataException($"Release の一覧は {Path.GetFileName(entry.Url)} を指していますが、添付のイメージは {r.Image.Name} です");
            var sums = Sha256Sums.Parse(await _downloader.FetchTextAsync(r.Sums!.DownloadUrl, ct));
            if (!sums.TryGetValue(r.Image.Name, out var expected))
                throw new InvalidDataException($"SHA256SUMS.txt に {r.Image.Name} がありません");
            if (entry.DownloadSha256 is not null && entry.DownloadSha256 != expected)
                throw new InvalidDataException("Release の一覧の image_download_sha256 と SHA256SUMS.txt が一致しません");
            if (entry.DownloadSize != r.Image.Size)
                throw new InvalidDataException($"Release の一覧のサイズ {entry.DownloadSize} と添付のサイズ {r.Image.Size} が一致しません");

            // The image: reuse a verified download, otherwise download and verify.
            var have = false;
            if (File.Exists(imagePath) && new FileInfo(imagePath).Length == r.Image.Size)
            {
                Status("ダウンロード済みのイメージを検証中…");
                var digest = await Sha256Sums.OfFileAsync(imagePath, new Progress<long>(n => DownloadBar.Value = 1000.0 * n / r.Image.Size), ct);
                have = digest == expected;
                if (!have) _log($"{r.Image.Name} はダウンロード済みですが SHA-256 が一致しないため取り直します");
            }
            if (!have)
            {
                if (!download) throw new FileNotFoundException("検証済みのイメージがありません。「ダウンロードして Imager で書き込む」を使ってください", imagePath);
                var sw = Stopwatch.StartNew();
                var progress = new Progress<DownloadProgress>(p =>
                {
                    if (p.Total is > 0) DownloadBar.Value = 1000.0 * p.Done / p.Total.Value;
                    var rate = sw.Elapsed.TotalSeconds > 0.5 ? p.Done / sw.Elapsed.TotalSeconds / (1024 * 1024) : 0;
                    Status($"ダウンロード中… {p.Done / (1024.0 * 1024.0):F0} / {(p.Total ?? r.Image.Size) / (1024.0 * 1024.0):F0} MB（{rate:F1} MB/s）");
                });
                await _downloader.DownloadFileAsync(r.Image.DownloadUrl, imagePath, r.Image.Size, progress, ct);
                Status("ダウンロードしたイメージを検証中…");
                var digest = await Sha256Sums.OfFileAsync(imagePath, new Progress<long>(n => DownloadBar.Value = 1000.0 * n / r.Image.Size), ct);
                if (digest != expected)
                {
                    File.Delete(imagePath);
                    throw new InvalidDataException($"{r.Image.Name} の SHA-256 が SHA256SUMS.txt と一致しません（ダウンロードが壊れています。もう一度実行してください）");
                }
                _log($"{r.Image.Name} をダウンロードし、SHA-256 を確認しました（{_imagesFolder}）");
            }

            // The manifest for Imager on this PC: the Release's entry pointing at the local file.
            await File.WriteAllTextAsync(manifestPath, ImagerManifest.Localize(manifestJson, imagePath), ct);
            DownloadBar.Value = 1000;
            Status("Raspberry Pi Imager を起動中…");
            var started = ImagerLauncher.Launch(manifestPath);
            _log($"Raspberry Pi Imager を開きました（{started}、一覧 {Path.GetFileName(manifestPath)}）");
            Status($"Imager を開きました。Imager で デバイス「Raspberry Pi Zero 2 W」→ OS「{entry.Name}」→ ストレージ（SD カード）を選び、書き込みます。"
                   + "Imager が管理者権限の確認を出したら許可してください。書き込みと検証が終わったら SD カードを車に入れます");
        }
        catch (OperationCanceledException)
        {
            Status("中止しました");
        }
        catch (Exception ex)
        {
            Status("失敗: " + ex.Message);
            _log("SD カードの作成: " + ex.Message);
        }
        finally
        {
            _cts = null;
            cts.Dispose();
            CancelButton.IsEnabled = false;
            FetchButton.IsEnabled = true;
            DownloadBar.Visibility = Visibility.Collapsed;
            var status = CreateStatus.Text;
            OnReleaseSelected(this, null);
            CreateStatus.Text = status;   // keep the result on screen, not the selection hint
        }
    }

    private void Status(string text) => CreateStatus.Text = text;

    private void OnOpenFolderClick(object sender, RoutedEventArgs e)
    {
        try
        {
            Directory.CreateDirectory(_imagesFolder);
            Process.Start(new ProcessStartInfo("explorer.exe", _imagesFolder) { UseShellExecute = true });
        }
        catch (Exception ex)
        {
            CreateStatus.Text = "フォルダを開けません: " + ex.Message;
        }
    }

    private void OnCopyUrlClick(object sender, RoutedEventArgs e)
    {
        try { Clipboard.SetText(ReleaseCatalog.LatestManifestUrl); } catch (Exception) { /* another application holds the clipboard */ }
    }
}
