using System.Runtime.InteropServices;
using System.Text;

namespace RumiCar.DriveManager;

/// <summary>
/// The Wi-Fi network this PC is connected to, through the Windows Native Wi-Fi
/// API (wlanapi.dll), so the drive manager can show it and warn when Windows
/// has moved the PC off the car's access point (docs 6.2).
///
/// Windows 11 24H2 and later hide the network name from desktop applications
/// unless 設定 → プライバシーとセキュリティ → 位置情報 →「デスクトップ アプリに
/// 位置情報へのアクセスを許可する」 is on; the query then fails with access
/// denied or returns no name, which is reported as <see cref="Kind.Hidden"/>.
/// </summary>
public static class PcWifi
{
    public enum Kind
    {
        Connected,      // Ssid holds the network name
        Disconnected,   // a Wi-Fi adapter, not connected
        NoWifi,         // no Wi-Fi adapter, or the WLAN service is not running
        Hidden,         // connected, but Windows does not give the name (location permission)
        Error,
    }

    public sealed record Status(Kind Kind, string Ssid, string Detail);

    private const uint ErrorAccessDenied = 5;
    private const uint ErrorServiceNotActive = 1062;
    private const int InterfaceInfoSize = 16 + 512 + 4;    // WLAN_INTERFACE_INFO: GUID, WCHAR[256], state
    private const int InterfaceStateOffset = 16 + 512;
    private const int StateConnected = 1;                  // wlan_interface_state_connected
    private const int OpcodeCurrentConnection = 7;         // wlan_intf_opcode_current_connection
    private const int ProfileNameOffset = 8;               // WLAN_CONNECTION_ATTRIBUTES.strProfileName (WCHAR[256])
    private const int SsidLengthOffset = 8 + 512;          // ... .wlanAssociationAttributes.dot11Ssid.uSSIDLength
    private const int SsidOffset = SsidLengthOffset + 4;   // ... .dot11Ssid.ucSSID[32]

    /// <summary>Queries the current connection (a few milliseconds; call it off the UI thread).</summary>
    public static Status Query()
    {
        uint err;
        IntPtr handle;
        try
        {
            err = WlanOpenHandle(2, IntPtr.Zero, out _, out handle);
        }
        catch (DllNotFoundException)
        {
            return new Status(Kind.NoWifi, "", "wlanapi.dll がありません");
        }
        if (err == ErrorServiceNotActive) return new Status(Kind.NoWifi, "", "WLAN AutoConfig サービスが動いていません");
        if (err != 0) return new Status(Kind.Error, "", $"WlanOpenHandle: {err}");
        try
        {
            err = WlanEnumInterfaces(handle, IntPtr.Zero, out var list);
            if (err != 0) return new Status(Kind.Error, "", $"WlanEnumInterfaces: {err}");
            try
            {
                var count = Marshal.ReadInt32(list, 0);
                if (count == 0) return new Status(Kind.NoWifi, "", "Wi-Fi のアダプターがありません");
                var hidden = false;
                for (var i = 0; i < count; i++)
                {
                    var item = list + 8 + i * InterfaceInfoSize;
                    if (Marshal.ReadInt32(item, InterfaceStateOffset) != StateConnected) continue;
                    var guid = Marshal.PtrToStructure<Guid>(item);
                    err = WlanQueryInterface(handle, ref guid, OpcodeCurrentConnection, IntPtr.Zero, out _, out var data, IntPtr.Zero);
                    if (err == ErrorAccessDenied) { hidden = true; continue; }
                    if (err != 0) continue;
                    try
                    {
                        var length = Marshal.ReadInt32(data, SsidLengthOffset);
                        if (length is > 0 and <= 32)
                        {
                            var bytes = new byte[length];
                            Marshal.Copy(data + SsidOffset, bytes, 0, length);
                            return new Status(Kind.Connected, Encoding.UTF8.GetString(bytes), "");
                        }
                        // No name in the association: the profile name is usually the same.
                        var profile = Marshal.PtrToStringUni(data + ProfileNameOffset, 256).TrimEnd('\0');
                        if (profile.Length > 0) return new Status(Kind.Connected, profile, "プロファイル名");
                        hidden = true;
                    }
                    finally
                    {
                        WlanFreeMemory(data);
                    }
                }
                return hidden
                    ? new Status(Kind.Hidden, "", "Windows がネットワーク名を渡しません（位置情報の許可）")
                    : new Status(Kind.Disconnected, "", "");
            }
            finally
            {
                WlanFreeMemory(list);
            }
        }
        finally
        {
            WlanCloseHandle(handle, IntPtr.Zero);
        }
    }

    [DllImport("wlanapi.dll")]
    private static extern uint WlanOpenHandle(uint clientVersion, IntPtr reserved, out uint negotiatedVersion, out IntPtr clientHandle);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanCloseHandle(IntPtr clientHandle, IntPtr reserved);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanEnumInterfaces(IntPtr clientHandle, IntPtr reserved, out IntPtr interfaceList);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanQueryInterface(IntPtr clientHandle, ref Guid interfaceGuid, int opCode, IntPtr reserved,
        out uint dataSize, out IntPtr data, IntPtr opcodeValueType);

    [DllImport("wlanapi.dll")]
    private static extern void WlanFreeMemory(IntPtr memory);
}
