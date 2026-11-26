using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

internal static class FanTargetStartup
{
    private const uint FanSetIoctl = 0x8002a414;
    private const uint OpenExisting = 3;
    private const int TargetC = 60;
    private const string DevicePath = @"\\.\Ps5Pwr";
    private const string LogPath = @"C:\ps5-work\fan-target-startup-fast.jsonl";

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateFileW(string name, uint access, uint share,
        IntPtr securityAttributes, uint creation, uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(IntPtr handle, uint code,
        byte[] input, int inputLength, byte[] output, int outputLength,
        out int returned, IntPtr overlapped);

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);

    private static void Log(string eventName, int attempt, string detail)
    {
        try
        {
            string safe = (detail ?? "").Replace("\\", "\\\\").Replace("\"", "'");
            string row = "{\"at\":\"" + DateTime.Now.ToString("o") +
                "\",\"event\":\"" + eventName + "\",\"target_c\":" + TargetC +
                ",\"attempt\":" + attempt + ",\"detail\":\"" + safe + "\"}" + Environment.NewLine;
            File.AppendAllText(LogPath, row, Encoding.ASCII);
        }
        catch { }
    }

    private static int Main()
    {
        DateTime deadline = DateTime.UtcNow.AddSeconds(90);
        int attempt = 0;
        string lastError = "device not ready";

        while (DateTime.UtcNow < deadline)
        {
            attempt++;
            IntPtr handle = CreateFileW(DevicePath, 0xC0000000, 0, IntPtr.Zero,
                OpenExisting, 0, IntPtr.Zero);

            if (handle == new IntPtr(-1))
            {
                lastError = "CreateFile win32=" + Marshal.GetLastWin32Error();
            }
            else
            {
                try
                {
                    byte[] input = BitConverter.GetBytes((uint)TargetC);
                    byte[] output = new byte[32];
                    int returned;
                    if (!DeviceIoControl(handle, FanSetIoctl, input, input.Length,
                        output, output.Length, out returned, IntPtr.Zero))
                    {
                        lastError = "DeviceIoControl win32=" + Marshal.GetLastWin32Error();
                    }
                    else if (returned < 32)
                    {
                        lastError = "short reply=" + returned;
                    }
                    else
                    {
                        uint target = BitConverter.ToUInt32(output, 0);
                        uint replyBytes = BitConverter.ToUInt32(output, 4);
                        if (target == TargetC && replyBytes >= 32)
                        {
                            Log("verified", attempt, "ioctl_bytes=" + returned + ";reply_bytes=" + replyBytes);
                            return 0;
                        }
                        lastError = "readback_target=" + target + ";reply_bytes=" + replyBytes;
                    }
                }
                finally { CloseHandle(handle); }
            }

            if (attempt == 1 || attempt % 20 == 0) Log("retry", attempt, lastError);
            Thread.Sleep(250);
        }

        Log("failed", attempt, lastError);
        return 1;
    }
}
