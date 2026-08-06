// Count how often a window's pixels actually change, from outside it.
//
// An application's own frame counter reports what it drew, not what reached the
// screen -- and in this project those differ by construction: VirtualGL renders
// in the guest, spoils frames it cannot ship in time, and an X server blits
// whatever did arrive. glxgears saying "34 FPS" is a claim about glxgears, not
// about the glass.
//
// So this asks the desktop instead: grab a small patch of the window over and
// over, hash each grab, and count how often the hash changes. Changes per
// second is the displayed frame rate, measured by something with no connection
// to the guest, the daemon or the X server.
//
// Raw GDI rather than System.Drawing, because the first version used
// Graphics.CopyFromScreen and managed 30 samples a second -- slower than the
// thing it was measuring, so it could only ever report a floor. One memory DC
// and one DIB section are created up front; the loop is BitBlt plus a hash over
// bits that are already in our address space.
//
// The patch is a strip through the middle of the window: small enough to grab
// quickly, and positioned where a spinning object changes every frame, which a
// corner might not.
//
// The sampling rate is reported next to the answer and must be read with it.
// Sampling at N/s cannot resolve more than N/2 fps, and if the measured rate
// approaches that ceiling the number is a floor rather than a measurement.
//
//   csc /nologo /unsafe /out:framerate.exe framerate.cs
//   framerate.exe <title-substring> [seconds]
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

class FrameRate
{
	[DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr p);
	delegate bool EnumProc(IntPtr hwnd, IntPtr p);
	[DllImport("user32.dll", CharSet = CharSet.Auto)]
	static extern int GetWindowText(IntPtr hwnd, StringBuilder s, int max);
	[DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
	[DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
	[DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr hwnd);
	[DllImport("user32.dll")] static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);

	[DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
	[DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
	[DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr o);
	[DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr o);
	[DllImport("gdi32.dll")]
	static extern bool BitBlt(IntPtr dst, int x, int y, int w, int h,
				  IntPtr src, int sx, int sy, int rop);
	[DllImport("gdi32.dll")]
	static extern IntPtr CreateDIBSection(IntPtr dc, ref BITMAPINFO bmi, uint usage,
					      out IntPtr bits, IntPtr section, uint offset);

	[StructLayout(LayoutKind.Sequential)]
	struct RECT { public int Left, Top, Right, Bottom; }

	[StructLayout(LayoutKind.Sequential)]
	struct BITMAPINFOHEADER
	{
		public uint biSize;
		public int biWidth, biHeight;
		public ushort biPlanes, biBitCount;
		public uint biCompression, biSizeImage;
		public int biXPelsPerMeter, biYPelsPerMeter;
		public uint biClrUsed, biClrImportant;
	}

	[StructLayout(LayoutKind.Sequential)]
	struct BITMAPINFO
	{
		public BITMAPINFOHEADER bmiHeader;
		public int bmiColors;
	}

	const int SRCCOPY = 0x00CC0020;
	const uint DIB_RGB_COLORS = 0;

	static string want;
	static IntPtr found = IntPtr.Zero;
	static string foundTitle = "";

	static bool Look(IntPtr hwnd, IntPtr p)
	{
		if (!IsWindowVisible(hwnd))
			return true;
		StringBuilder sb = new StringBuilder(512);
		GetWindowText(hwnd, sb, sb.Capacity);
		string t = sb.ToString();
		if (t.Length > 0 && t.IndexOf(want, StringComparison.OrdinalIgnoreCase) >= 0) {
			found = hwnd; foundTitle = t; return false;
		}
		return true;
	}

	unsafe static int Main(string[] args)
	{
		want = args.Length > 0 ? args[0] : "glxgears";
		double secs = args.Length > 1 ? double.Parse(args[1]) : 5.0;

		EnumWindows(new EnumProc(Look), IntPtr.Zero);
		if (found == IntPtr.Zero) {
			Console.WriteLine("framerate: no visible window matching \"{0}\"", want);
			return 1;
		}

		RECT r;
		GetWindowRect(found, out r);
		int w = r.Right - r.Left, h = r.Bottom - r.Top;
		Console.WriteLine("window  : \"{0}\"  {1}x{2} at {3},{4}",
				  foundTitle, w, h, r.Left, r.Top);

		int sw = Math.Min(w - 8, 128), sh = 4;
		if (sw < 8) { Console.WriteLine("framerate: window too small"); return 1; }

		// Straight from the window's own DC, so the strip is in client
		// coordinates and the measurement follows the window if it moves.
		IntPtr wdc = GetDC(found);
		IntPtr mdc = CreateCompatibleDC(wdc);

		BITMAPINFO bmi = new BITMAPINFO();
		bmi.bmiHeader.biSize = (uint)Marshal.SizeOf(typeof(BITMAPINFOHEADER));
		bmi.bmiHeader.biWidth = sw;
		bmi.bmiHeader.biHeight = -sh;		/* top-down */
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = 0;	/* BI_RGB */

		IntPtr bits;
		IntPtr dib = CreateDIBSection(wdc, ref bmi, DIB_RGB_COLORS, out bits,
					      IntPtr.Zero, 0);
		if (dib == IntPtr.Zero) {
			Console.WriteLine("framerate: CreateDIBSection failed");
			return 1;
		}
		SelectObject(mdc, dib);

		int sx = (w - sw) / 2, sy = h / 2;
		Console.WriteLine("sampling: {0}x{1} strip at client {2},{3} for {4:F1} s",
				  sw, sh, sx, sy, secs);

		long samples = 0, changes = 0;
		int prev = 0;
		bool first = true;
		int nbytes = sw * sh * 4;
		Stopwatch clk = Stopwatch.StartNew();

		while (clk.Elapsed.TotalSeconds < secs) {
			BitBlt(mdc, 0, 0, sw, sh, wdc, sx, sy, SRCCOPY);

			int hash = unchecked((int)2166136261);
			byte* p = (byte*)bits;
			for (int i = 0; i < nbytes; i++) {
				hash ^= p[i];
				hash *= 16777619;
			}

			if (first) first = false;
			else if (hash != prev) changes++;
			prev = hash;
			samples++;
		}

		double el = clk.Elapsed.TotalSeconds;
		double rate = samples / el, fps = changes / el;

		Console.WriteLine();
		Console.WriteLine("samples : {0} in {1:F2} s = {2:F0} samples/s"
				  + "  (can resolve up to {3:F0} fps)",
				  samples, el, rate, rate / 2.0);
		Console.WriteLine("changes : {0}", changes);
		Console.WriteLine();
		Console.WriteLine("DISPLAYED FRAME RATE = {0:F1} fps", fps);
		if (fps > rate / 2.5)
			Console.WriteLine("WARNING: near the sampling ceiling --"
					  + " treat as a floor, not a measurement");

		DeleteObject(dib);
		DeleteDC(mdc);
		ReleaseDC(found, wdc);
		return 0;
	}
}
