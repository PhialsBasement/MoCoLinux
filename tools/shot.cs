// Capture the whole XP desktop to a PNG.
//
// There is no screenshot tool on this machine and no way to drive one over the
// transfer agent, but .NET 4 is installed and ships csc.exe, so the shortest
// path to a picture is to compile one. Built and run on the box; the file comes
// back through the agent like any other.
//
// SystemInformation.VirtualScreen rather than PrimaryScreen.Bounds so a
// multi-monitor desktop is captured whole, which is also what makes the
// rootless X windows show up wherever they happen to be.
//
//   csc /nologo /target:winexe /out:shot.exe shot.cs
//   shot.exe <path.png>
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Windows.Forms;

class Shot
{
	[STAThread]
	static int Main(string[] args)
	{
		string path = args.Length > 0 ? args[0] : "shot.png";
		Rectangle r = SystemInformation.VirtualScreen;

		using (Bitmap bmp = new Bitmap(r.Width, r.Height, PixelFormat.Format32bppArgb))
		{
			using (Graphics g = Graphics.FromImage(bmp))
			{
				// CopyFromScreen takes the composited desktop, which is
				// the point: the guest's windows are real windows here,
				// so they arrive with their XP frames and the taskbar.
				g.CopyFromScreen(r.X, r.Y, 0, 0, r.Size, CopyPixelOperation.SourceCopy);
			}
			bmp.Save(path, ImageFormat.Png);
		}

		Console.WriteLine("shot: {0}x{1} -> {2}", r.Width, r.Height, path);
		return 0;
	}
}
