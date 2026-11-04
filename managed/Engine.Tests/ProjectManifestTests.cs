// ProjectManifestTests.cs — P7-05: project.json `packages` schema acceptance.
//
// Parsing a project with packages succeeds; missing/invalid fields fail
// validation; absent/empty packages stays backward compatible.
using Engine.Editor;

namespace Engine.Tests;

public static class ProjectManifestTests
{
    public static int Run()
    {
        var failures = 0;
        failures += Check(ParsesWithPackages(), "manifest: parses project with packages");
        failures += Check(AbsentPackagesCompatible(), "manifest: absent packages is backward compatible");
        failures += Check(EmptyPackagesCompatible(), "manifest: empty packages is backward compatible");
        failures += Check(MissingFieldsFail(), "manifest: missing/invalid fields fail validation");
        failures += Check(RoundTrip(), "manifest: save/load round-trips packages");
        Console.WriteLine(failures == 0 ? "ProjectManifestTests: all checks passed" : $"ProjectManifestTests: {failures} FAILED");
        return failures;
    }

    private static bool ParsesWithPackages()
    {
        const string json = """
            {"name":"MyGame","startScene":"scenes/main.scene.json","gameAssembly":"bin/Game.dll",
             "packages":[{"name":"core","url":"https://example.com/core.git","tag":"v1.2.3"},
                         {"name":"fx-pack","url":"https://example.com/fx.git","tag":"v0.9.0"}]}
            """;
        ProjectManifest m;
        try
        {
            m = ProjectManifestFile.Parse(json);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  unexpected parse failure: {ex.Message}");
            return false;
        }

        return m.Name == "MyGame"
            && m.StartScene == "scenes/main.scene.json"
            && m.GameAssembly == "bin/Game.dll"
            && m.PackageList.Count == 2
            && m.PackageList[0] == new PackageReference("core", "https://example.com/core.git", "v1.2.3")
            && m.PackageList[1].Name == "fx-pack";
    }

    private static bool AbsentPackagesCompatible()
    {
        try
        {
            var m = ProjectManifestFile.Parse("""{"name":"MarbleRun","gameAssembly":"bin/Release/net10.0/MarbleRun.dll"}""");
            return m.Name == "MarbleRun" && m.PackageList.Count == 0 && m.StartScene is null;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  unexpected parse failure: {ex.Message}");
            return false;
        }
    }

    private static bool EmptyPackagesCompatible()
    {
        try
        {
            var m = ProjectManifestFile.Parse("""{"name":"G","packages":[]}""");
            return m.PackageList.Count == 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  unexpected parse failure: {ex.Message}");
            return false;
        }
    }

    private static bool MissingFieldsFail()
    {
        string[] bad =
        [
            """{"startScene":"s"}""", // missing name
            """{"name":""}""", // empty name
            """{"name":"G","packages":{}}""", // packages not array
            """{"name":"G","packages":[{}]}""", // entry missing all fields
            """{"name":"G","packages":[{"name":"a","url":"https://e.com/a.git"}]}""", // missing tag
            """{"name":"G","packages":[{"name":"a","url":"https://e.com/a.git","tag":""}]}""", // empty tag
            """{"name":"G","packages":[{"name":"bad name!","url":"https://e.com/a.git","tag":"v1"}]}""", // bad name chars
            """{"name":"G","packages":[{"name":"a","url":"git@example.com:a.git","tag":"v1"}]}""", // non-https url
            """{"name":"G","packages":[{"name":"a","url":"https://e.com/a.git","tag":"v1"},{"name":"a","url":"https://e.com/b.git","tag":"v2"}]}""", // duplicate
            """{"name":"G","packages":[{"name":"a","url":"https://e.com/a.git","tag":"v1","extra":1}]}""", // unknown package field
            """{"name":"G","bogus":1}""", // unknown root field
            """{"name":"G","packages":null}""", // null packages
            """[1,2]""", // non-object root
            """not json""", // malformed
        ];
        foreach (var json in bad)
        {
            try
            {
                ProjectManifestFile.Parse(json);
            }
            catch (ProjectManifestException)
            {
                continue;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"  wrong exception for {json}: {ex.GetType().Name}");
                return false;
            }

            Console.WriteLine($"  accepted invalid manifest: {json}");
            return false;
        }

        return true;
    }

    private static bool RoundTrip()
    {
        var dir = Directory.CreateTempSubdirectory("manifest-test-").FullName;
        try
        {
            var path = Path.Combine(dir, "project.json");
            var original = new ProjectManifest(
                "MyGame", "scenes/main.scene.json", "bin/Game.dll",
                [new PackageReference("core", "https://example.com/core.git", "v1.2.3")]);
            ProjectManifestFile.Save(path, original);
            var loaded = ProjectManifestFile.Load(path);
            return loaded.Name == original.Name
                && loaded.StartScene == original.StartScene
                && loaded.GameAssembly == original.GameAssembly
                && loaded.PackageList.SequenceEqual(original.PackageList)
                && ProjectManifestFile.Parse(ProjectManifestFile.Serialize(loaded)).PackageList
                    .SequenceEqual(original.PackageList);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  round-trip failure: {ex.Message}");
            return false;
        }
        finally
        {
            try
            {
                foreach (var f in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories))
                {
                    try
                    {
                        File.SetAttributes(f, FileAttributes.Normal);
                    }
                    catch (IOException)
                    {
                    }
                    catch (UnauthorizedAccessException)
                    {
                    }
                }

                Directory.Delete(dir, recursive: true);
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
        }
    }

    private static int Check(bool ok, string name)
    {
        if (!ok)
        {
            Console.WriteLine($"[FAIL] {name}");
            return 1;
        }

        Console.WriteLine($"[PASS] {name}");
        return 0;
    }
}
