// ============================================================
// ShieldCord — DesktopCapture.cs
//
// Screen capture of one small region of the desktop, used to prove the
// "Test Mode" watermark is actually gone rather than assuming the hook
// worked.
//
// WHY THIS EXISTS AT ALL: the watermark is painted by shell32 inside
// explorer, and every hook we install could silently fail to match — a
// wrong resource ID, a cached string, a render path we did not
// anticipate. Without looking at pixels, all of those failures report as
// success. This is the only check in the feature that cannot lie.
//
// IT CANNOT ALWAYS LOOK, THOUGH. If the desktop is covered, the region
// shows whatever is on top of it. Callers must treat a null from
// Capture as "unverified", never as "clean".
// ============================================================

using System;
using System.Runtime.InteropServices;

namespace ShieldCordUI.Services;

/// <summary>
/// A rectangle in physical screen pixels.
/// </summary>
internal readonly record struct CaptureRect(int X, int Y, int Width, int Height);

internal static class DesktopCapture
{
    /// <summary>
    /// Where Windows paints the desktop watermark, in physical pixels.
    ///
    /// Bottom-right of the primary monitor, inset from the corner. The box
    /// is deliberately generous: it only has to CONTAIN the text, because
    /// the comparison is before-versus-after over the same rectangle, so an
    /// oversized box costs a few thousand wasted pixels and nothing else.
    /// A box that is too small would clip the text and let a half-removed
    /// watermark read as removed.
    /// </summary>
    public static CaptureRect WatermarkRegion()
    {
        // DPI-aware (the manifest sets PerMonitorV2), so these are real
        // device pixels rather than DIPs, and so is the capture below.
        double scale = 1.0;
        try
        {
            uint dpi = GetDpiForSystem();
            if (dpi >= 48 && dpi <= 480) scale = dpi / 96.0;
        }
        catch (EntryPointNotFoundException)
        {
            // Pre-1607. 96 is the right guess there.
        }

        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);

        int w = (int)Math.Ceiling(420 * scale);
        int h = (int)Math.Ceiling(80 * scale);
        int margin = (int)Math.Ceiling(4 * scale);

        // Clamp so a tiny or oddly-scaled screen still yields a usable rect.
        if (w > screenW) w = screenW;
        if (h > screenH) h = screenH;

        return new CaptureRect(
            X: Math.Max(0, screenW - w - margin),
            Y: Math.Max(0, screenH - h - margin),
            Width: w,
            Height: h);
    }

    /// <summary>
    /// Grab the region, or null if it could not be read.
    ///
    /// Null is the honest answer for "I could not look", and callers must
    /// keep it distinct from "I looked and it was clean".
    /// </summary>
    public static byte[]? Capture(CaptureRect rect)
    {
        if (rect.Width <= 0 || rect.Height <= 0) return null;

        IntPtr screenDc = IntPtr.Zero;
        IntPtr memDc = IntPtr.Zero;
        IntPtr bitmap = IntPtr.Zero;
        IntPtr oldBitmap = IntPtr.Zero;

        try
        {
            // GetDC(NULL) is the whole virtual screen - the composited
            // desktop, which is what actually shows the watermark.
            screenDc = GetDC(IntPtr.Zero);
            if (screenDc == IntPtr.Zero) return null;

            memDc = CreateCompatibleDC(screenDc);
            if (memDc == IntPtr.Zero) return null;

            bitmap = CreateCompatibleBitmap(screenDc, rect.Width, rect.Height);
            if (bitmap == IntPtr.Zero) return null;

            oldBitmap = SelectObject(memDc, bitmap);

            if (!BitBlt(memDc, 0, 0, rect.Width, rect.Height,
                        screenDc, rect.X, rect.Y, SRCCOPY | CAPTUREBLT))
            {
                return null;
            }

            var info = new BITMAPINFO
            {
                bmiHeader = new BITMAPINFOHEADER
                {
                    biSize = (uint)Marshal.SizeOf<BITMAPINFOHEADER>(),
                    biWidth = rect.Width,
                    // Negative height = top-down rows, which keeps the
                    // comparison orientation-stable without a flip.
                    biHeight = -rect.Height,
                    biPlanes = 1,
                    biBitCount = 32,
                    biCompression = BI_RGB,
                },
            };

            int stride = rect.Width * 4;
            byte[] pixels = new byte[stride * rect.Height];
            int scanned = GetDIBits(memDc, bitmap, 0, (uint)rect.Height,
                                    pixels, ref info, DIB_RGB_COLORS);
            return scanned == 0 ? null : pixels;
        }
        catch
        {
            // A failed capture is not worth throwing over; it degrades the
            // result to "unverified", which the caller already handles.
            return null;
        }
        finally
        {
            if (oldBitmap != IntPtr.Zero && memDc != IntPtr.Zero) SelectObject(memDc, oldBitmap);
            if (bitmap != IntPtr.Zero) DeleteObject(bitmap);
            if (memDc != IntPtr.Zero) DeleteDC(memDc);
            if (screenDc != IntPtr.Zero) ReleaseDC(IntPtr.Zero, screenDc);
        }
    }

    /// <summary>
    /// Percent of pixels that changed between two captures of the same
    /// region, or -1 when they are not comparable.
    ///
    /// A per-channel threshold absorbs the small wobble from DWM
    /// compositing and any wallpaper dithering, so that "the watermark
    /// text disappeared" reads as a large change and "nothing happened"
    /// reads as zero rather than as sensor noise.
    /// </summary>
    public static double PercentChanged(byte[]? before, byte[]? after)
    {
        if (before is null || after is null) return -1;
        if (before.Length != after.Length || before.Length == 0) return -1;

        int changed = 0;
        for (int i = 0; i + 3 < before.Length; i += 4)
        {
            int db = Math.Abs(before[i]     - after[i]);
            int dg = Math.Abs(before[i + 1] - after[i + 1]);
            int dr = Math.Abs(before[i + 2] - after[i + 2]);
            if (db > ChannelThreshold || dg > ChannelThreshold || dr > ChannelThreshold)
            {
                changed++;
            }
        }

        int pixels = before.Length / 4;
        return pixels == 0 ? -1 : (100.0 * changed / pixels);
    }

    /// <summary>
    /// Is the region actually showing the DESKTOP, or is something covering it?
    ///
    /// This is what lets the caller tell "we removed it" from "we could not
    /// look". Without it, a full-screen window over the region makes the
    /// before and after captures identical — which would be misread as a
    /// failed removal, when the truth is that nothing could be observed.
    ///
    /// The test is on the TOP-LEVEL window at each sample point, so a small
    /// overlay (a tooltip, a notification) does not get to answer for the
    /// whole region: several points have to agree.
    /// </summary>
    public static bool IsDesktopVisibleAt(CaptureRect rect)
    {
        IntPtr shell = GetShellWindow();
        if (shell == IntPtr.Zero) return false;

        // Centre plus the four inset corners.
        int left = rect.X + rect.Width / 4;
        int right = rect.X + (rect.Width * 3) / 4;
        int top = rect.Y + rect.Height / 4;
        int bottom = rect.Y + (rect.Height * 3) / 4;

        Span<(int X, int Y)> samples = stackalloc (int, int)[]
        {
            (rect.X + rect.Width / 2, rect.Y + rect.Height / 2),
            (left, top), (right, top), (left, bottom), (right, bottom),
        };

        foreach (var (x, y) in samples)
        {
            var point = new POINT { X = x, Y = y };
            IntPtr hwnd = WindowFromPoint(point);
            if (hwnd == IntPtr.Zero) return false;

            IntPtr root = GetAncestor(hwnd, GA_ROOT);
            if (root == shell) continue;

            // On Windows 11 the desktop surface is hosted in a WorkerW while
            // GetShellWindow() reports Progman, so requiring the root to BE the
            // shell window declared a plainly visible desktop "covered" — which
            // is how a removal that had actually worked got reported as
            // unverified. The window class is the reliable test.
            if (IsDesktopClass(root)) continue;

            return false;
        }
        return true;
    }

    private static bool IsDesktopClass(IntPtr hwnd)
    {
        if (hwnd == IntPtr.Zero) return false;

        var name = new char[64];
        int len = GetClassNameW(hwnd, name, name.Length);
        if (len <= 0) return false;

        string cls = new string(name, 0, len);
        return cls.Equals("Progman", StringComparison.OrdinalIgnoreCase)
            || cls.Equals("WorkerW", StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// Ask the shell to repaint the desktop, so a capture taken now shows the
    /// watermark as it will be drawn from here on. Returns false if nothing
    /// could be nudged.
    ///
    /// RE-APPLYING THE CURRENT WALLPAPER is what actually makes this happen.
    /// Invalidating the desktop window is NOT enough on Windows 10+: the
    /// wallpaper and the watermark are no longer painted by the window a naive
    /// RedrawWindow reaches, so that approach appears to work and silently does
    /// nothing — which is exactly how the first version of this shipped, and
    /// why the watermark stayed on screen with the hooks correctly installed.
    ///
    /// The wallpaper path is read back first and written unchanged, so nothing
    /// the user can see changes; it only asks for a repaint.
    /// </summary>
    public static bool ForceRepaint()
    {
        try
        {
            var path = new System.Text.StringBuilder(260);
            if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, (uint)path.Capacity, path, 0)
                && path.Length > 0)
            {
                SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, path.ToString(),
                                      SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
                return true;
            }
        }
        catch
        {
            // Fall through to the invalidate below.
        }

        // A slideshow or Spotlight background has no path to re-apply, so this
        // is all that is left - and the capture will say honestly whether it
        // was enough.
        try
        {
            IntPtr desktop = GetDesktopWindow();
            if (desktop != IntPtr.Zero)
            {
                RedrawWindow(desktop, IntPtr.Zero, IntPtr.Zero,
                             RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
                return true;
            }
        }
        catch
        {
            // Best effort: a failed repaint shows up as "unverified", not as a lie.
        }

        return false;
    }

    /// <summary>
    /// Capture once the region has stopped changing.
    ///
    /// A wallpaper re-apply can fade on Windows 11, and a frame grabbed
    /// mid-transition differs from the baseline in every pixel — which would
    /// read as "the watermark was removed" for entirely the wrong reason. Only
    /// two consecutive identical frames are worth comparing.
    /// </summary>
    public static byte[]? CaptureStable(CaptureRect rect, int attempts = 8, int intervalMs = 180)
    {
        byte[]? previous = Capture(rect);
        for (int i = 1; i < attempts; i++)
        {
            System.Threading.Thread.Sleep(intervalMs);
            byte[]? next = Capture(rect);
            if (previous is not null && next is not null && PercentChanged(previous, next) == 0.0)
            {
                return next;
            }
            previous = next;
        }
        return previous;
    }

    // ── constants ────────────────────────────────────────────────
    private const int SM_CXSCREEN = 0;
    private const int SM_CYSCREEN = 1;
    private const int SRCCOPY     = 0x00CC0020;
    private const int CAPTUREBLT  = 0x40000000;
    private const uint BI_RGB     = 0;
    private const uint DIB_RGB_COLORS = 0;
    private const int ChannelThreshold = 24;
    private const uint GA_ROOT = 2;

    private const uint RDW_INVALIDATE = 0x0001;
    private const uint RDW_ERASE      = 0x0004;
    private const uint RDW_ALLCHILDREN = 0x0080;
    private const uint RDW_UPDATENOW  = 0x0100;

    private const uint SPI_GETDESKWALLPAPER = 0x0073;
    private const uint SPI_SETDESKWALLPAPER = 0x0014;
    private const uint SPIF_UPDATEINIFILE   = 0x0001;
    private const uint SPIF_SENDCHANGE      = 0x0002;

    // ── interop ──────────────────────────────────────────────────
    // Classic DllImport with W-suffixed entry points, matching the house
    // style in ServiceControl.cs. No unsafe, no LibraryImport.

    [StructLayout(LayoutKind.Sequential)]
    private struct BITMAPINFOHEADER
    {
        public uint biSize;
        public int biWidth;
        public int biHeight;
        public ushort biPlanes;
        public ushort biBitCount;
        public uint biCompression;
        public uint biSizeImage;
        public int biXPelsPerMeter;
        public int biYPelsPerMeter;
        public uint biClrUsed;
        public uint biClrImportant;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct BITMAPINFO
    {
        public BITMAPINFOHEADER bmiHeader;
        public uint bmiColors;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT
    {
        public int X;
        public int Y;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetDC(IntPtr hWnd);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern int ReleaseDC(IntPtr hWnd, IntPtr hDc);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetDesktopWindow();

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetShellWindow();

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr WindowFromPoint(POINT point);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetAncestor(IntPtr hWnd, uint flags);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern int GetClassNameW(IntPtr hWnd, [Out] char[] className, int maxCount);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool RedrawWindow(IntPtr hWnd, IntPtr lprcUpdate,
                                            IntPtr hrgnUpdate, uint flags);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern int GetSystemMetrics(int nIndex);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetDpiForSystem();

    // Two overloads because the same call is used both to read the current
    // wallpaper path into a buffer and to write the identical path back.
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool SystemParametersInfoW(uint action, uint param,
                                                     System.Text.StringBuilder value, uint flags);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool SystemParametersInfoW(uint action, uint param,
                                                     string value, uint flags);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern IntPtr CreateCompatibleDC(IntPtr hDc);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern IntPtr CreateCompatibleBitmap(IntPtr hDc, int width, int height);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern IntPtr SelectObject(IntPtr hDc, IntPtr hObject);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern bool DeleteObject(IntPtr hObject);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern bool DeleteDC(IntPtr hDc);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern bool BitBlt(IntPtr hDcDest, int x, int y, int width, int height,
                                      IntPtr hDcSrc, int srcX, int srcY, int rop);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern int GetDIBits(IntPtr hDc, IntPtr hBitmap, uint start,
                                        uint lines, byte[] bits, ref BITMAPINFO info,
                                        uint usage);
}
