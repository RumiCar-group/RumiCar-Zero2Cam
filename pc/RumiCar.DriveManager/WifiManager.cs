using System.Runtime.InteropServices;
using System.Text;
using System.Xml.Linq;

namespace RumiCar.DriveManager;

/// <summary>
/// The PC's Wi-Fi through the Windows Native Wi-Fi API (wlanapi.dll), for
/// putting the PC back on the car's network (docs 6.2): notifications of the
/// auto configuration module (connected, disconnected, connection failed), the
/// saved profile of the car's network, and connecting with that profile.
///
/// None of these calls is among those that Windows 11 24H2 and later refuse
/// without location permission (that applies to the current connection, the
/// scan and the lists of networks and BSSs; <see cref="PcWifi"/>), so they work
/// whether or not the network name is shown.  The profile itself is not
/// changed: the PC joins the car's network with the profile Windows saved
/// when the user first connected to it.
/// </summary>
public sealed class WifiManager : IDisposable
{
    public enum NoticeKind
    {
        /// <summary>A connection completed (<see cref="Notice.Reason"/> 0) or failed (otherwise).</summary>
        ConnectionComplete,
        ConnectionFailed,
        Disconnected,
    }

    /// <summary>One notification: the interface, the profile (and SSID when Windows gives it), the reason as text.</summary>
    public sealed record Notice(Guid Interface, NoticeKind Kind, string Profile, string Ssid, uint ReasonCode, string Reason);

    /// <summary>The car's network on one interface: the profile to connect with.</summary>
    public sealed record Target(Guid Interface, string InterfaceName, string Profile, string Ssid);

    /// <summary>Raised on a thread of the WLAN service; the handler must not block.</summary>
    public event Action<Notice>? NoticeReceived;

    private const uint ClientVersion = 2;
    private const uint SourceNone = 0;
    private const uint SourceAcm = 0x8;                     // WLAN_NOTIFICATION_SOURCE_ACM
    private const uint AcmConnectionComplete = 10;          // wlan_notification_acm_connection_complete
    private const uint AcmConnectionAttemptFail = 11;       // wlan_notification_acm_connection_attempt_fail
    private const uint AcmDisconnected = 21;                // wlan_notification_acm_disconnected
    private const int InterfaceInfoSize = 16 + 512 + 4;     // WLAN_INTERFACE_INFO: GUID, WCHAR[256], state
    private const int InterfaceStateOffset = 16 + 512;
    private const int StateConnected = 1;                   // wlan_interface_state_connected
    private const int ProfileInfoSize = 512 + 4;            // WLAN_PROFILE_INFO: WCHAR[256], flags
    // WLAN_CONNECTION_NOTIFICATION_DATA: mode, WCHAR[256] profile, DOT11_SSID, BSS type, security, reason code, flags, XML
    private const int NoticeProfileOffset = 4;
    private const int NoticeSsidLengthOffset = 4 + 512;
    private const int NoticeSsidOffset = NoticeSsidLengthOffset + 4;
    private const int NoticeReasonOffset = NoticeSsidOffset + 32 + 4 + 4;
    private const int ConnectionModeProfile = 0;            // wlan_connection_mode_profile
    private const int BssTypeInfrastructure = 1;            // dot11_BSS_type_infrastructure
    private static readonly XNamespace ProfileNs = "http://www.microsoft.com/networking/WLAN/profile/v1";

    private readonly IntPtr _handle;
    private readonly WlanNotificationCallback _callback;   // kept alive while registered
    private int _disposed;

    private WifiManager(IntPtr handle)
    {
        _handle = handle;
        _callback = OnNotification;
    }

    /// <summary>Opens the WLAN service and starts the notifications; null with the reason when there is no Wi-Fi.</summary>
    public static WifiManager? TryOpen(out string problem)
    {
        uint err;
        IntPtr handle;
        try
        {
            err = WlanOpenHandle(ClientVersion, IntPtr.Zero, out _, out handle);
        }
        catch (DllNotFoundException)
        {
            problem = "wlanapi.dll がありません";
            return null;
        }
        if (err != 0)
        {
            problem = err == 1062 ? "WLAN AutoConfig サービスが停止しています" : $"WlanOpenHandle: {err}";
            return null;
        }
        var manager = new WifiManager(handle);
        err = WlanRegisterNotification(handle, SourceAcm, true, manager._callback, IntPtr.Zero, IntPtr.Zero, out _);
        if (err != 0)
        {
            WlanCloseHandle(handle, IntPtr.Zero);
            problem = $"WlanRegisterNotification: {err}";
            return null;
        }
        problem = "";
        return manager;
    }

    /// <summary>
    /// The saved profile of the network <paramref name="ssid"/>, on an interface
    /// that is connected if there is one.  Null with the reason when no profile
    /// of that network is saved.
    /// </summary>
    public Target? FindTarget(string ssid, out string problem)
    {
        var candidates = new List<(Target Target, bool Connected)>();
        foreach (var (iface, name, connected) in Interfaces())
        {
            foreach (var profile in ProfileNames(iface))
            {
                if (ProfileSsid(iface, profile) == ssid) candidates.Add((new Target(iface, name, profile, ssid), connected));
            }
        }
        problem = candidates.Count == 0 ? $"Wi-Fi のプロファイルに {ssid} がありません（一度 Windows の Wi-Fi 一覧から {ssid} に接続してください）" : "";
        // A connected interface first; on one interface, the profile named as the network (as Windows names it) first.
        return candidates.OrderByDescending(c => c.Connected).ThenByDescending(c => c.Target.Profile == ssid).Select(c => c.Target).FirstOrDefault();
    }

    /// <summary>Asks Windows to join the network with the profile; the outcome comes as a notification.  "" or the error.</summary>
    public string Connect(Target target)
    {
        var parameters = new WlanConnectionParameters
        {
            ConnectionMode = ConnectionModeProfile,
            Profile = target.Profile,
            Dot11Ssid = IntPtr.Zero,
            DesiredBssidList = IntPtr.Zero,
            BssType = BssTypeInfrastructure,
            Flags = 0,
        };
        var iface = target.Interface;
        var err = WlanConnect(_handle, ref iface, ref parameters, IntPtr.Zero);
        return err == 0 ? "" : $"WlanConnect: {err}";
    }

    /// <summary>Leaves the network and joins it again (as a user does by hand).  "" or the error.</summary>
    public async Task<string> RejoinAsync(Target target)
    {
        var iface = target.Interface;
        var err = WlanDisconnect(_handle, ref iface, IntPtr.Zero);
        if (err != 0) return $"WlanDisconnect: {err}";
        await Task.Delay(1000);   // the adapter leaves the network before the new connection is asked for
        return Connect(target);
    }

    private List<(Guid Interface, string Name, bool Connected)> Interfaces()
    {
        var result = new List<(Guid, string, bool)>();
        if (WlanEnumInterfaces(_handle, IntPtr.Zero, out var list) != 0) return result;
        try
        {
            var count = Marshal.ReadInt32(list, 0);
            for (var i = 0; i < count; i++)
            {
                var item = list + 8 + i * InterfaceInfoSize;
                var guid = Marshal.PtrToStructure<Guid>(item);
                var name = Marshal.PtrToStringUni(item + 16) ?? "";
                var connected = Marshal.ReadInt32(item, InterfaceStateOffset) == StateConnected;
                result.Add((guid, name, connected));
            }
        }
        finally
        {
            WlanFreeMemory(list);
        }
        return result;
    }

    private List<string> ProfileNames(Guid iface)
    {
        var result = new List<string>();
        if (WlanGetProfileList(_handle, ref iface, IntPtr.Zero, out var list) != 0) return result;
        try
        {
            var count = Marshal.ReadInt32(list, 0);
            for (var i = 0; i < count; i++)
            {
                var name = Marshal.PtrToStringUni(list + 8 + i * ProfileInfoSize);
                if (!string.IsNullOrEmpty(name)) result.Add(name);
            }
        }
        finally
        {
            WlanFreeMemory(list);
        }
        return result;
    }

    /// <summary>The network name in the profile's XML (its name element, otherwise its hex element).</summary>
    private string? ProfileSsid(Guid iface, string profile)
    {
        uint flags = 0;
        if (WlanGetProfile(_handle, ref iface, profile, IntPtr.Zero, out var xmlPtr, ref flags, out _) != 0) return null;
        try
        {
            var xml = Marshal.PtrToStringUni(xmlPtr);
            if (string.IsNullOrEmpty(xml)) return null;
            var ssid = XDocument.Parse(xml).Root?.Element(ProfileNs + "SSIDConfig")?.Element(ProfileNs + "SSID");
            var name = ssid?.Element(ProfileNs + "name")?.Value;
            if (!string.IsNullOrEmpty(name)) return name;
            var hex = ssid?.Element(ProfileNs + "hex")?.Value;
            return string.IsNullOrEmpty(hex) ? null : Encoding.UTF8.GetString(Convert.FromHexString(hex));
        }
        catch (Exception)
        {
            return null;   // a profile that cannot be read is not the car's
        }
        finally
        {
            WlanFreeMemory(xmlPtr);
        }
    }

    private void OnNotification(IntPtr dataPtr, IntPtr context)
    {
        if (Volatile.Read(ref _disposed) != 0 || dataPtr == IntPtr.Zero) return;
        try
        {
            var data = Marshal.PtrToStructure<WlanNotificationData>(dataPtr);
            if (data.Source != SourceAcm) return;
            if (data.Code is not (AcmConnectionComplete or AcmConnectionAttemptFail or AcmDisconnected)) return;
            if (data.Data == IntPtr.Zero || data.DataSize < NoticeReasonOffset + 4) return;
            var profile = Marshal.PtrToStringUni(data.Data + NoticeProfileOffset) ?? "";
            var ssidLength = Math.Clamp(Marshal.ReadInt32(data.Data, NoticeSsidLengthOffset), 0, 32);
            var ssidBytes = new byte[ssidLength];
            Marshal.Copy(data.Data + NoticeSsidOffset, ssidBytes, 0, ssidLength);
            var reason = (uint)Marshal.ReadInt32(data.Data, NoticeReasonOffset);
            var kind = data.Code switch
            {
                AcmDisconnected => NoticeKind.Disconnected,
                AcmConnectionAttemptFail => NoticeKind.ConnectionFailed,
                _ => reason == 0 ? NoticeKind.ConnectionComplete : NoticeKind.ConnectionFailed,
            };
            NoticeReceived?.Invoke(new Notice(data.InterfaceGuid, kind, profile, Encoding.UTF8.GetString(ssidBytes), reason, ReasonText(reason)));
        }
        catch (Exception)
        {
            // A notification that cannot be read is dropped; the 2 s poll of the connection still runs.
        }
    }

    /// <summary>The reason code in Windows' words (in the language of Windows); "" for success.</summary>
    private static string ReasonText(uint reason)
    {
        if (reason == 0) return "";
        var buffer = new StringBuilder(512);
        return WlanReasonCodeToString(reason, (uint)buffer.Capacity, buffer, IntPtr.Zero) == 0 && buffer.Length > 0
            ? buffer.ToString()
            : $"理由コード {reason}";
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        WlanRegisterNotification(_handle, SourceNone, true, null, IntPtr.Zero, IntPtr.Zero, out _);
        WlanCloseHandle(_handle, IntPtr.Zero);
        GC.KeepAlive(_callback);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WlanNotificationData
    {
        public uint Source;
        public uint Code;
        public Guid InterfaceGuid;
        public uint DataSize;
        public IntPtr Data;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WlanConnectionParameters
    {
        public int ConnectionMode;
        [MarshalAs(UnmanagedType.LPWStr)] public string Profile;
        public IntPtr Dot11Ssid;
        public IntPtr DesiredBssidList;
        public int BssType;
        public uint Flags;
    }

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate void WlanNotificationCallback(IntPtr data, IntPtr context);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanOpenHandle(uint clientVersion, IntPtr reserved, out uint negotiatedVersion, out IntPtr handle);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanCloseHandle(IntPtr handle, IntPtr reserved);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanRegisterNotification(IntPtr handle, uint notifSource, bool ignoreDuplicate,
        WlanNotificationCallback? callback, IntPtr context, IntPtr reserved, out uint prevNotifSource);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanEnumInterfaces(IntPtr handle, IntPtr reserved, out IntPtr interfaceList);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanGetProfileList(IntPtr handle, ref Guid interfaceGuid, IntPtr reserved, out IntPtr profileList);

    [DllImport("wlanapi.dll", CharSet = CharSet.Unicode)]
    private static extern uint WlanGetProfile(IntPtr handle, ref Guid interfaceGuid, string profileName, IntPtr reserved,
        out IntPtr profileXml, ref uint flags, out uint grantedAccess);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanConnect(IntPtr handle, ref Guid interfaceGuid, ref WlanConnectionParameters parameters, IntPtr reserved);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanDisconnect(IntPtr handle, ref Guid interfaceGuid, IntPtr reserved);

    [DllImport("wlanapi.dll", CharSet = CharSet.Unicode)]
    private static extern uint WlanReasonCodeToString(uint reasonCode, uint bufferSize, StringBuilder stringBuffer, IntPtr reserved);

    [DllImport("wlanapi.dll")]
    private static extern void WlanFreeMemory(IntPtr memory);
}
