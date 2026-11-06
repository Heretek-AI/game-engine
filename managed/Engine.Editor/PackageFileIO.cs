// PackageFileIO.cs — atomic file replacement for package state files.
//
// packages.lock.json and project.json are read by UI polls (snapshot,
// asset listing) while Resolve/Save writes them. An in-place
// File.WriteAllText truncates first, so a concurrent reader can see a
// half-written file. Write to a temp file in the same directory, then
// move over the target in one step (same-directory rename).
namespace Engine.Editor;

internal static class PackageFileIO
{
    public static void WriteAllTextAtomic(string path, string content)
    {
        var full = Path.GetFullPath(path);
        var dir = Path.GetDirectoryName(full) ?? ".";
        var tmp = Path.Combine(dir, ".tmp-" + Path.GetRandomFileName());
        try
        {
            File.WriteAllText(tmp, content);
            File.Move(tmp, full, overwrite: true);
        }
        catch
        {
            try
            {
                File.Delete(tmp);
            }
            catch (Exception)
            {
                // Best-effort temp cleanup; the original error propagates.
            }

            throw;
        }
    }
}
