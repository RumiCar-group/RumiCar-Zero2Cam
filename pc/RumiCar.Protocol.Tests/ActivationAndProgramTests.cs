using System.Text.Json;
using RumiCar.Protocol.Control;
using RumiCar.Protocol.Packs;
using Xunit;

namespace RumiCar.Protocol.Tests;

public class ActivationPlannerTests
{
    private static readonly List<InstalledVersion> Installed = new()
    {
        new("logic", "tof_drive-0.2.0", "tof_drive", "0.2.0", true),
        new("logic", "cam_drive-0.2.0", "cam_drive", "0.2.0", true),
        new("logic", "py_drive-0.1.0", "py_drive", "0.1.0", true),
        new("pack", "base-tof-0.1.1", "base-tof", "0.1.1", true, "tof_drive"),
        new("pack", "base-tof-0.1.2", "base-tof", "0.1.2", true, "tof_drive"),
        new("pack", "cam-base-0.1.0", "cam-base", "0.1.0", true, "cam_drive"),
        new("pack", "py-sample-0.1.0", "py-sample", "0.1.0", true, "py_drive"),
        new("pack", "my-drive-20261023.101500", "my-drive", "20261023.101500", true, "py_drive"),
        new("pack", "my-drive-20261022.090000", "my-drive", "20261022.090000", true, "py_drive"),
        new("pack", "broken-0.1.0", "broken", "0.1.0", false, "tof_drive"),
    };

    private static InstalledVersion Get(string name) => Installed.First(v => v.DirectoryName == name);

    [Fact]
    public void APackOfTheRunningLogicIsActivatedAlone()
    {
        var s = ActivationPlanner.Plan(Get("base-tof-0.1.2"), Installed, "tof_drive-0.2.0", "base-tof-0.1.1");
        Assert.True(s.IsSingle);
        Assert.Equal("pack", s.Kind);
        Assert.Equal("base-tof-0.1.2", s.Name);
    }

    [Fact]
    public void APackOfAnotherLogicBringsItsLogic()
    {
        var s = ActivationPlanner.Plan(Get("py-sample-0.1.0"), Installed, "tof_drive-0.2.0", "base-tof-0.1.1");
        Assert.True(s.IsPair);
        Assert.Equal("py_drive-0.1.0", s.Logic);
        Assert.Equal("py-sample-0.1.0", s.Pack);
        Assert.NotNull(s.Note);
    }

    [Fact]
    public void ALogicOfAnotherFamilyBringsItsNewestPack()
    {
        var s = ActivationPlanner.Plan(Get("py_drive-0.1.0"), Installed, "tof_drive-0.2.0", "base-tof-0.1.1");
        Assert.True(s.IsPair);
        Assert.Equal("py_drive-0.1.0", s.Logic);
        Assert.Equal("my-drive-20261023.101500", s.Pack);   // newest py_drive pack (numeric version order)
        var back = ActivationPlanner.Plan(Get("tof_drive-0.2.0"), Installed, "py_drive-0.1.0", "py-sample-0.1.0");
        Assert.Equal("base-tof-0.1.2", back.Pack);
    }

    [Fact]
    public void ALogicOfTheSameFamilyIsActivatedAlone()
    {
        var s = ActivationPlanner.Plan(Get("tof_drive-0.2.0"), Installed, "tof_drive-0.2.0", "base-tof-0.1.1");
        Assert.True(s.IsSingle);
        Assert.Equal("logic", s.Kind);
    }

    [Fact]
    public void MissingHalvesAndBrokenVersionsAreReported()
    {
        var onlyTof = Installed.Where(v => v.Id != "py_drive").ToList();
        var s = ActivationPlanner.Plan(Get("py-sample-0.1.0"), onlyTof, "tof_drive-0.2.0", "base-tof-0.1.1");
        Assert.NotNull(s.Error);
        Assert.False(s.IsSingle || s.IsPair);
        var noPack = Installed.Where(v => v.Logic != "cam_drive").ToList();
        Assert.NotNull(ActivationPlanner.Plan(Get("cam_drive-0.2.0"), noPack, "tof_drive-0.2.0", "base-tof-0.1.1").Error);
        Assert.NotNull(ActivationPlanner.Plan(Get("broken-0.1.0"), Installed, "tof_drive-0.2.0", "base-tof-0.1.1").Error);
    }

    [Fact]
    public void AnOlderCarWithoutTheLogicFieldGetsSingleSteps()
    {
        var old = Installed.Select(v => v with { Logic = "" }).ToList();
        var s = ActivationPlanner.Plan(old.First(v => v.DirectoryName == "py-sample-0.1.0"), old, "tof_drive-0.2.0", "base-tof-0.1.1");
        Assert.True(s.IsSingle);
    }

    [Fact]
    public void RollbackAcrossFamiliesTakesBoth()
    {
        Assert.True(ActivationPlanner.RollbackNeedsBoth("logic", Installed, "py_drive-0.1.0", "py-sample-0.1.0", "tof_drive-0.2.0", "base-tof-0.1.1"));
        Assert.True(ActivationPlanner.RollbackNeedsBoth("pack", Installed, "py_drive-0.1.0", "py-sample-0.1.0", "tof_drive-0.2.0", "base-tof-0.1.1"));
        Assert.False(ActivationPlanner.RollbackNeedsBoth("pack", Installed, "tof_drive-0.2.0", "base-tof-0.1.2", "tof_drive-0.2.0", "base-tof-0.1.1"));
        Assert.False(ActivationPlanner.RollbackNeedsBoth("logic", Installed, "tof_drive-0.2.0", "base-tof-0.1.2", "", ""));
    }

    [Theory]
    [InlineData("0.10.0", "0.9.0", 1)]
    [InlineData("20261023.101500", "20261022.235959", 1)]
    [InlineData("0.2.0", "0.2.0", 0)]
    [InlineData("0.2", "0.2.1", -1)]
    public void VersionsCompareNumerically(string a, string b, int sign)
    {
        Assert.Equal(sign, Math.Sign(ActivationPlanner.VersionComparer.Instance.Compare(a, b)));
    }
}

public class ProgramPackTests
{
    private static string NewFolder(string name)
    {
        var dir = Path.Combine(Path.GetTempPath(), "rumicar-prog-" + Guid.NewGuid().ToString("N"), name);
        Directory.CreateDirectory(dir);
        return dir;
    }

    [Theory]
    [InlineData("My Drive_2", "my-drive-2")]
    [InlineData("py-sample", "py-sample")]
    [InlineData("自分の走行", "my-drive")]
    [InlineData("--abc--", "abc")]
    public void PackIdsAreSafe(string folder, string expected)
    {
        Assert.Equal(expected, ProgramPack.PackId(folder));
    }

    [Fact]
    public void AFolderBecomesAVerifiedPackForPyDrive()
    {
        var src = NewFolder("My Drive");
        File.WriteAllText(Path.Combine(src, "drive.py"), "def loop(car):\n    car.drive(500)\n");
        File.WriteAllText(Path.Combine(src, "params.json"), "{\"speed\": 500}");
        File.WriteAllText(Path.Combine(src, "helper.py"), "X = 1\n");
        File.WriteAllText(Path.Combine(src, "notes.txt"), "not sent");
        File.WriteAllText(Path.Combine(src, "manifest.json"), "{}");
        Directory.CreateDirectory(Path.Combine(src, "__pycache__"));
        File.WriteAllText(Path.Combine(src, "__pycache__", "drive.cpython-313.pyc"), "x");
        var staging = Path.Combine(Path.GetDirectoryName(src)!, "staging");
        var r = ProgramPack.Prepare(src, staging, new DateTime(2026, 10, 23, 10, 15, 0));
        Assert.Equal("my-drive", r.Id);
        Assert.Equal("20261023.101500", r.Version);
        Assert.Equal(new[] { "drive.py", "helper.py", "params.json" }, r.Files.ToArray());
        var (manifest, problems) = ManifestVerifier.Verify(r.StagingDirectory);
        Assert.Empty(problems);
        Assert.NotNull(manifest);
        Assert.Equal("my-drive-20261023.101500", manifest!.DirectoryName);
        Assert.Equal("py_drive", manifest.Logic!.Name);
        Assert.Equal(2, manifest.Logic.MinAbi);
        Assert.Contains("My Drive", manifest.Description);
        var plan = VersionPush.Prepare(r.StagingDirectory);
        Assert.Equal("pack", plan.Kind);
        Assert.Equal(4, plan.Files.Count);   // three files and manifest.json
        Assert.Equal("{}", File.ReadAllText(Path.Combine(src, "manifest.json")));   // the user's folder is untouched
    }

    [Fact]
    public void MistakesAreReportedInWords()
    {
        var src = NewFolder("p");
        var e = Assert.Throws<InvalidDataException>(() => ProgramPack.Prepare(src, src + "-s", DateTime.Now));
        Assert.Contains("drive.py", e.Message);
        File.WriteAllText(Path.Combine(src, "drive.py"), "def loop(car):\n    pass\n");
        File.WriteAllText(Path.Combine(src, "params.json"), "{\n  \"speed\": 600 600\n}");
        e = Assert.Throws<InvalidDataException>(() => ProgramPack.Prepare(src, src + "-s", DateTime.Now));
        Assert.Contains("params.json の 2 行目", e.Message);
        File.WriteAllText(Path.Combine(src, "params.json"), "[1, 2]");
        e = Assert.Throws<InvalidDataException>(() => ProgramPack.Prepare(src, src + "-s", DateTime.Now));
        Assert.Contains("{ ... }", e.Message);
    }
}

public class NewRequestTests
{
    private static JsonElement Params(ControlRequest r)
    {
        Assert.True(ControlCodec.TryDecode(ControlCodec.Encode(r), out var back));
        var req = Assert.IsType<ControlRequest>(back);
        return JsonSerializer.SerializeToElement(req.Params);
    }

    [Fact]
    public void AutoLimitPairAndRollbackBoth()
    {
        var p = Params(ControlCodec.SetAutoLimit(1, 600));
        Assert.Equal(600, p.GetProperty("drive_permille").GetInt32());
        Assert.Throws<ArgumentOutOfRangeException>(() => ControlCodec.SetAutoLimit(1, 1001));
        p = Params(ControlCodec.ActivatePair(2, "py_drive-0.1.0", "py-sample-0.1.0"));
        Assert.Equal("py_drive-0.1.0", p.GetProperty("logic").GetString());
        Assert.Equal("py-sample-0.1.0", p.GetProperty("pack").GetString());
        Assert.Throws<ArgumentException>(() => ControlCodec.ActivatePair(2, "../x", "y"));
        p = Params(ControlCodec.RollbackBoth(3));
        Assert.Equal("both", p.GetProperty("kind").GetString());
    }
}
