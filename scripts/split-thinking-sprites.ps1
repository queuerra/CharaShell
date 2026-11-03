param(
    [string]$SourcePath = (Join-Path $PSScriptRoot '../resources/thinking/cutout-source.png'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../resources/thinking')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$drawingReferences = @('System.Drawing.Common', 'System.Drawing.Primitives', 'System.Collections')
if (Test-Path -LiteralPath (Join-Path $PSHOME 'System.Private.Windows.GdiPlus.dll')) {
    $drawingReferences += 'System.Private.Windows.GdiPlus', 'System.Private.Windows.Core'
}
Add-Type -ReferencedAssemblies $drawingReferences -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Drawing.Drawing2D;
using System.IO;

public static class ThinkingSpriteSplitter {
    const int Side = 272;
    static Bitmap Extract(Bitmap source, int index) {
        int start = (int)Math.Round(index * source.Width / 8.0);
        int end = (int)Math.Round((index + 1) * source.Width / 8.0);
        var crop = new Rectangle(start + 5, 242, end - start - 10, 252);
        var frame = new Bitmap(Side, Side, PixelFormat.Format32bppArgb);
        using (var graphics = Graphics.FromImage(frame)) {
            graphics.Clear(Color.Transparent);
            graphics.CompositingMode = CompositingMode.SourceCopy;
            graphics.DrawImage(source, new Rectangle((Side - crop.Width) / 2, 10, crop.Width, crop.Height), crop, GraphicsUnit.Pixel);
        }
        // The crop excludes the grid. Keep the cloud and its two trailing
        // circles, excluding detached residue and thin edge noise.
        var core = new bool[Side * Side];
        for (int y = 1; y < Side - 1; ++y) for (int x = 1; x < Side - 1; ++x) {
            bool solid = true;
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
                if (frame.GetPixel(x + dx, y + dy).A < 48) solid = false;
            core[y * Side + x] = solid;
        }
        var seen = new bool[core.Length];
        var components = new List<List<int>>();
        for (int i = 0; i < core.Length; ++i) {
            if (!core[i] || seen[i]) continue;
            var component = new List<int>(); var queue = new Queue<int>();
            queue.Enqueue(i); seen[i] = true;
            while (queue.Count > 0) {
                int p = queue.Dequeue(); component.Add(p); int x = p % Side, y = p / Side;
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= Side || ny >= Side) continue;
                    int n = ny * Side + nx;
                    if (!seen[n] && core[n]) { seen[n] = true; queue.Enqueue(n); }
                }
            }
            if (component.Count > 80) components.Add(component);
        }
        components.Sort((a, b) => b.Count.CompareTo(a.Count));
        var keep = new bool[core.Length];
        for (int i = 0; i < Math.Min(3, components.Count); ++i) foreach (int p in components[i]) {
            int x = p % Side, y = p / Side;
            // Recover the original antialiased outline around each component.
            for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) {
                int nx = x + dx, ny = y + dy;
                if (nx >= 0 && ny >= 0 && nx < Side && ny < Side) keep[ny * Side + nx] = true;
            }
        }
        for (int y = 0; y < Side; ++y) for (int x = 0; x < Side; ++x)
            if (!keep[y * Side + x]) frame.SetPixel(x, y, Color.Transparent);
        return frame;
    }
    public static void Split(string sourcePath, string outputDirectory) {
        Directory.CreateDirectory(outputDirectory);
        using (var source = new Bitmap(sourcePath))
        using (var sheet = new Bitmap(Side * 8, Side, PixelFormat.Format32bppArgb))
        using (var graphics = Graphics.FromImage(sheet)) {
            graphics.Clear(Color.Transparent);
            graphics.CompositingMode = CompositingMode.SourceCopy;
            for (int i = 0; i < 8; ++i) using (var frame = Extract(source, i)) {
                frame.Save(Path.Combine(outputDirectory, "frame-" + i.ToString("D2") + ".png"), ImageFormat.Png);
                graphics.DrawImageUnscaled(frame, i * Side, 0);
            }
            sheet.Save(Path.Combine(outputDirectory, "sprite-sheet.png"), ImageFormat.Png);
        }
    }
}
'@
[ThinkingSpriteSplitter]::Split([IO.Path]::GetFullPath($SourcePath), [IO.Path]::GetFullPath($OutputDirectory))
