using System;
using System.Runtime.InteropServices;
using Windows.Foundation;
using Windows.Graphics.Capture;
using Windows.Graphics.DirectX;
using Windows.Graphics.DirectX.Direct3D11;
using Windows.Graphics.Imaging;

// One window's own composed pixels through Windows Graphics Capture: other
// windows, notifications and overlays above it are not in them. Compiled by
// operator-host.ps1 with the .NET Framework compiler against the system
// WinMetadata (todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 27).
public static class WindowCapture {
  [ComImport, Guid("3628E81B-3CAC-4C60-B7F4-23CE0E0C3356"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IGraphicsCaptureItemInterop {
    [PreserveSig] int CreateForWindow(IntPtr w, ref Guid iid, out IntPtr item);
    [PreserveSig] int CreateForMonitor(IntPtr m, ref Guid iid, out IntPtr item);
  }
  [DllImport("d3d11.dll")] static extern int D3D11CreateDevice(IntPtr adapter, int type, IntPtr sw, uint flags, IntPtr levels, uint n, uint sdk, out IntPtr dev, out int level, out IntPtr ctx);
  [DllImport("d3d11.dll")] static extern int CreateDirect3D11DeviceFromDXGIDevice(IntPtr dxgi, out IntPtr insp);
  [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int a, out Rect v, int n);
  struct Rect { public int Left, Top, Right, Bottom; }
  [DllImport("combase.dll")] static extern int RoGetActivationFactory(IntPtr cls, ref Guid iid, out IntPtr f);
  [DllImport("combase.dll", CharSet = CharSet.Unicode)] static extern int WindowsCreateString(string s, int n, out IntPtr h);
  [DllImport("combase.dll")] static extern int WindowsDeleteString(IntPtr h);

  [ComImport, Guid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  unsafe interface IMemoryBufferByteAccess { void GetBuffer(out byte* data, out uint capacity); }

  static void Check(int hr, string what) { if (hr < 0) throw new Exception(what + " failed: 0x" + hr.ToString("X8")); }

  // The window's pixels (BGRA, opaque, top-down rows of width*4 bytes) and the
  // screen position of their top-left corner (the window's visible frame).
  public static byte[] Capture(IntPtr hwnd, int timeoutMs, out int width, out int height, out int left, out int top) {
    Rect bounds;
    Check(DwmGetWindowAttribute(hwnd, 9, out bounds, 16), "the frame bounds");   // DWMWA_EXTENDED_FRAME_BOUNDS
    left = bounds.Left; top = bounds.Top;
    IntPtr dev, ctx, dxgi, insp, fac, itemp; int level;
    Check(D3D11CreateDevice(IntPtr.Zero, 1, IntPtr.Zero, 0x20, IntPtr.Zero, 0, 7, out dev, out level, out ctx), "D3D11CreateDevice");
    Marshal.Release(ctx);
    Guid iidDxgi = new Guid("54ec77fa-1377-44e6-8c32-88fd5f44c84c");
    Check(Marshal.QueryInterface(dev, ref iidDxgi, out dxgi), "IDXGIDevice");
    Marshal.Release(dev);
    Check(CreateDirect3D11DeviceFromDXGIDevice(dxgi, out insp), "CreateDirect3D11DeviceFromDXGIDevice");
    Marshal.Release(dxgi);
    var device = (IDirect3DDevice)Marshal.GetObjectForIUnknown(insp);
    Marshal.Release(insp);
    IntPtr cls; string name = "Windows.Graphics.Capture.GraphicsCaptureItem";
    Check(WindowsCreateString(name, name.Length, out cls), "WindowsCreateString");
    Guid iidInterop = typeof(IGraphicsCaptureItemInterop).GUID;
    try { Check(RoGetActivationFactory(cls, ref iidInterop, out fac), "RoGetActivationFactory"); } finally { WindowsDeleteString(cls); }
    var interop = (IGraphicsCaptureItemInterop)Marshal.GetObjectForIUnknown(fac);
    Marshal.Release(fac);
    Guid iidItem = new Guid("79C3F95B-31F7-4EC2-A464-632EF5D30760");
    Check(interop.CreateForWindow(hwnd, ref iidItem, out itemp), "CreateForWindow");
    var item = (GraphicsCaptureItem)Marshal.GetObjectForIUnknown(itemp);
    Marshal.Release(itemp);
    using (var pool = Direct3D11CaptureFramePool.CreateFreeThreaded(device, DirectXPixelFormat.B8G8R8A8UIntNormalized, 2, item.Size))
    using (var session = pool.CreateCaptureSession(item)) {
      try { session.IsCursorCaptureEnabled = false; } catch (Exception) { }
      try { session.IsBorderRequired = false; } catch (Exception) { }
      session.StartCapture();
      var until = DateTime.UtcNow.AddMilliseconds(timeoutMs);
      Direct3D11CaptureFrame frame = null;
      while (frame == null) {
        frame = pool.TryGetNextFrame();
        if (frame == null) {
          if (DateTime.UtcNow > until) throw new Exception("no capture frame within " + timeoutMs + " ms");
          System.Threading.Thread.Sleep(20);
        }
      }
      var op = SoftwareBitmap.CreateCopyFromSurfaceAsync(frame.Surface, BitmapAlphaMode.Premultiplied);
      while (op.Status == AsyncStatus.Started) {
        if (DateTime.UtcNow > until) throw new Exception("the frame was not copied within " + timeoutMs + " ms");
        System.Threading.Thread.Sleep(10);
      }
      if (op.Status != AsyncStatus.Completed) throw new Exception("the frame copy ended " + op.Status);
      using (frame)
      using (var bmp = op.GetResults())
      using (var buf = bmp.LockBuffer(BitmapBufferAccessMode.Read))
      using (var reference = buf.CreateReference()) {
        width = bmp.PixelWidth; height = bmp.PixelHeight;
        if (width != bounds.Right - bounds.Left || height != bounds.Bottom - bounds.Top)
          throw new Exception("the frame is " + width + "x" + height + ", the window " + (bounds.Right - bounds.Left) + "x" + (bounds.Bottom - bounds.Top));
        var plane = buf.GetPlaneDescription(0);
        var bytes = new byte[width * height * 4];
        unsafe {
          byte* data; uint capacity;
          ((IMemoryBufferByteAccess)reference).GetBuffer(out data, out capacity);
          if ((long)plane.StartIndex + (long)plane.Stride * (height - 1) + width * 4 > capacity) throw new Exception("the frame buffer is smaller than its planes");
          for (int y = 0; y < height; y++) Marshal.Copy((IntPtr)(data + plane.StartIndex + (long)plane.Stride * y), bytes, y * width * 4, width * 4);
        }
        return bytes;
      }
    }
  }
}
