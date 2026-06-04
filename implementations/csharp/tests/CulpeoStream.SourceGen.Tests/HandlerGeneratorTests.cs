using System.Collections.Immutable;
using System.Reflection;
using CulpeoStream.SourceGen.Generators;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

namespace CulpeoStream.SourceGen.Tests;

/// <summary>
/// Unit tests for <see cref="Generators.HandlerGenerator"/> using Roslyn's
/// <see cref="CSharpGeneratorDriver"/> in-process.
/// </summary>
public sealed class HandlerGeneratorTests
{
    // ── Compilation helpers ───────────────────────────────────────────────────

    private static CSharpCompilation CreateBaseCompilation(string source)
    {
        var references = new List<MetadataReference>
        {
            MetadataReference.CreateFromFile(typeof(object).Assembly.Location),
            MetadataReference.CreateFromFile(typeof(Task).Assembly.Location),
            MetadataReference.CreateFromFile(typeof(IReadOnlyList<>).Assembly.Location),
            MetadataReference.CreateFromFile(Assembly.Load("System.Runtime").Location),
            MetadataReference.CreateFromFile(typeof(CulpeoStream.Core.CulpeoStreamType).Assembly.Location),
            MetadataReference.CreateFromFile(typeof(CulpeoStream.AspNetCore.ICulpeoStreamHandler).Assembly.Location),
        };

        // Add ASP.NET Core refs needed transitively
        TryAddRef(references, "Microsoft.AspNetCore.Http.Abstractions");
        TryAddRef(references, "Microsoft.AspNetCore.Http.Features");
        TryAddRef(references, "Microsoft.Extensions.Primitives");

        return CSharpCompilation.Create(
            "TestAssembly",
            new[] { CSharpSyntaxTree.ParseText(source) },
            references,
            new CSharpCompilationOptions(
                OutputKind.DynamicallyLinkedLibrary,
                nullableContextOptions: NullableContextOptions.Enable));
    }

    private static void TryAddRef(List<MetadataReference> refs, string assemblyName)
    {
        try { refs.Add(MetadataReference.CreateFromFile(Assembly.Load(assemblyName).Location)); }
        catch { /* optional */ }
    }

    private static (IReadOnlyDictionary<string, string> Sources, ImmutableArray<Diagnostic> Diagnostics)
        RunGenerator(string source)
    {
        var compilation = CreateBaseCompilation(source);
        var generator = new HandlerGenerator();
        var driver = CSharpGeneratorDriver.Create(generator);
        driver = (CSharpGeneratorDriver)driver.RunGenerators(compilation);
        var result = driver.GetRunResult();

        var sources = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var tree in result.GeneratedTrees)
            sources[System.IO.Path.GetFileName(tree.FilePath)] = tree.GetText().ToString();

        return (sources, result.Diagnostics);
    }

    private static ImmutableArray<Diagnostic> CompileWithGenerator(string source)
    {
        var compilation = CreateBaseCompilation(source);
        var generator = new HandlerGenerator();
        var driver = CSharpGeneratorDriver.Create(generator);
        driver = (CSharpGeneratorDriver)driver.RunGenerators(compilation);

        var updatedCompilation = driver.GetRunResult().GeneratedTrees
            .Aggregate(compilation, (c, t) => c.AddSyntaxTrees(t));

        return updatedCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToImmutableArray();
    }

    // ── Shared preamble for handler class test sources ────────────────────────

    private const string Preamble =
        "using CulpeoStream.AspNetCore;\n" +
        "using CulpeoStream.Core;\n" +
        "using CulpeoStream.Generated;\n" +
        "using System.Threading;\n" +
        "using System.Threading.Tasks;\n";

    private const string HandlerMembers =
        "    public Task<bool> AuthenticateAsync(string a, CancellationToken ct) => Task.FromResult(true);\n" +
        "    public Task OnConnectedAsync(ICulpeoStreamSession s, CancellationToken ct) => Task.CompletedTask;\n" +
        "    public Task OnMediaFrameAsync(ICulpeoStreamSession s, CulpeoMediaFrameContext f, CancellationToken ct) => Task.CompletedTask;\n" +
        "    public Task OnEventAsync(ICulpeoStreamSession s, CulpeoEventContext e, CancellationToken ct) => Task.CompletedTask;\n" +
        "    public Task OnDisconnectedAsync(ICulpeoStreamSession s, string? code, CancellationToken ct) => Task.CompletedTask;\n";

    // ── Tests: attribute emission ─────────────────────────────────────────────

    [Fact]
    public void AttributeSource_AlwaysEmitted()
    {
        var (sources, _) = RunGenerator(string.Empty);
        Assert.True(sources.ContainsKey("CulpeoStreamAttributes.g.cs"),
            "CulpeoStreamAttributes.g.cs was not emitted.");
    }

    // ── Tests: zero streams ───────────────────────────────────────────────────

    [Fact]
    public void ZeroStreams_GeneratesEmptyRegisteredStreams()
    {
        var source =
            Preamble +
            "[CulpeoStreamHandler]\n" +
            "public partial class ZeroStreamHandler : ICulpeoStreamHandler\n" +
            "{\n" +
            HandlerMembers +
            "}\n";

        var (sources, diagnostics) = RunGenerator(source);
        Assert.Empty(diagnostics.Where(d => d.Severity == DiagnosticSeverity.Error));
        Assert.True(sources.ContainsKey("ZeroStreamHandler_CulpeoGenerated.g.cs"));

        var generated = sources["ZeroStreamHandler_CulpeoGenerated.g.cs"];
        Assert.Contains("RegisteredStreams", generated);
        Assert.Contains("Array.Empty", generated);
        Assert.Contains("OnMessageAsync", generated);
        Assert.Contains("HandleMediaAsync", generated);
    }

    [Fact]
    public void ZeroStreams_GeneratedCodeCompiles()
    {
        var source =
            Preamble +
            "[CulpeoStreamHandler]\n" +
            "public partial class ZeroStreamHandler : ICulpeoStreamHandler\n" +
            "{\n" +
            HandlerMembers +
            "}\n";

        var errors = CompileWithGenerator(source);
        Assert.Empty(errors);
    }

    // ── Tests: one stream ─────────────────────────────────────────────────────

    [Fact]
    public void OneStream_GeneratesStreamInRegisteredStreams()
    {
        var source =
            Preamble +
            "[CulpeoStreamHandler]\n" +
            "public partial class OneStreamHandler : ICulpeoStreamHandler\n" +
            "{\n" +
            "    [DeclareStream(\"audio-in\", CulpeoStreamType.Input, OffsetType.Time, \"audio/pcm;rate=16000;channels=1;bits=16\")]\n" +
            "    private StreamDeclaration _audioIn = default!;\n" +
            HandlerMembers +
            "}\n";

        var (sources, diagnostics) = RunGenerator(source);
        Assert.Empty(diagnostics.Where(d => d.Severity == DiagnosticSeverity.Error));
        var generated = sources["OneStreamHandler_CulpeoGenerated.g.cs"];

        Assert.Contains("audio/pcm", generated);
        Assert.Contains("OnAudioInAsync", generated);
        Assert.DoesNotContain("Array.Empty", generated);

        // F1: must generate CulpeoRegisterStreamIds and use reverse-lookup dispatch
        Assert.Contains("CulpeoRegisterStreamIds", generated);
        Assert.Contains("_culpeo_streamIdMap", generated);
        Assert.Contains("audio-in", generated); // hint used in registration + dispatch
    }

    [Fact]
    public void OneStream_GeneratedCodeCompiles()
    {
        var source =
            Preamble +
            "[CulpeoStreamHandler]\n" +
            "public partial class OneStreamHandler : ICulpeoStreamHandler\n" +
            "{\n" +
            "    [DeclareStream(\"audio-in\", CulpeoStreamType.Input, OffsetType.Time, \"audio/pcm;rate=16000;channels=1;bits=16\")]\n" +
            "    private StreamDeclaration _audioIn = default!;\n" +
            HandlerMembers +
            "}\n";

        var errors = CompileWithGenerator(source);
        Assert.Empty(errors);
    }

    // ── Tests: three streams (mixed types) ────────────────────────────────────

    [Fact]
    public void ThreeStreams_AllStreamsInRegisteredStreams()
    {
        var source =
            Preamble +
            "[CulpeoStreamHandler]\n" +
            "public partial class ThreeStreamHandler : ICulpeoStreamHandler\n" +
            "{\n" +
            "    [DeclareStream(\"audio-in\", CulpeoStreamType.Input, OffsetType.Time, \"audio/pcm;rate=16000;channels=1;bits=16\", \"microphone\")]\n" +
            "    private StreamDeclaration _audioIn = default!;\n" +
            "    [DeclareStream(\"audio-out\", CulpeoStreamType.Output, OffsetType.Byte, \"audio/opus\", \"speaker\")]\n" +
            "    private StreamDeclaration _audioOut = default!;\n" +
            "    [DeclareStream(\"control\", CulpeoStreamType.Duplex, OffsetType.Message, \"application/json\")]\n" +
            "    private StreamDeclaration _control = default!;\n" +
            HandlerMembers +
            "}\n";

        var (sources, diagnostics) = RunGenerator(source);
        Assert.Empty(diagnostics.Where(d => d.Severity == DiagnosticSeverity.Error));
        var generated = sources["ThreeStreamHandler_CulpeoGenerated.g.cs"];

        Assert.Contains("audio-in", generated);
        Assert.Contains("audio-out", generated);
        Assert.Contains("control", generated);
        Assert.Contains("OnAudioInAsync", generated);
        Assert.Contains("OnAudioOutAsync", generated);
        Assert.Contains("OnControlAsync", generated);
        Assert.Contains("CulpeoStreamType.Input", generated);
        Assert.Contains("CulpeoStreamType.Output", generated);
        Assert.Contains("CulpeoStreamType.Duplex", generated);
    }

    [Fact]
    public void ThreeStreams_GeneratedCodeCompiles()
    {
        var source =
            Preamble +
            "[CulpeoStreamHandler]\n" +
            "public partial class ThreeStreamHandler : ICulpeoStreamHandler\n" +
            "{\n" +
            "    [DeclareStream(\"audio-in\", CulpeoStreamType.Input, OffsetType.Time, \"audio/pcm;rate=16000;channels=1;bits=16\", \"microphone\")]\n" +
            "    private StreamDeclaration _audioIn = default!;\n" +
            "    [DeclareStream(\"audio-out\", CulpeoStreamType.Output, OffsetType.Byte, \"audio/opus\", \"speaker\")]\n" +
            "    private StreamDeclaration _audioOut = default!;\n" +
            "    [DeclareStream(\"control\", CulpeoStreamType.Duplex, OffsetType.Message, \"application/json\")]\n" +
            "    private StreamDeclaration _control = default!;\n" +
            HandlerMembers +
            "}\n";

        var errors = CompileWithGenerator(source);
        Assert.Empty(errors);
    }

    // ── Tests: diagnostics ────────────────────────────────────────────────────

    [Fact]
    public void DuplicateStreamId_ProducesCULPEO001()
    {
        var source =
            Preamble +
            "[CulpeoStreamHandler]\n" +
            "public partial class DupIdHandler : ICulpeoStreamHandler\n" +
            "{\n" +
            "    [DeclareStream(\"audio\", CulpeoStreamType.Input, OffsetType.Byte, \"audio/opus\")]\n" +
            "    private StreamDeclaration _audioA = default!;\n" +
            "    [DeclareStream(\"audio\", CulpeoStreamType.Output, OffsetType.Byte, \"audio/opus\")]\n" +
            "    private StreamDeclaration _audioB = default!;\n" +
            HandlerMembers +
            "}\n";

        var (_, diagnostics) = RunGenerator(source);
        Assert.NotEmpty(diagnostics.Where(d => d.Id == "CULPEO001"));
    }

    [Fact]
    public void MissingInterface_ProducesCULPEO002_AndNoSource()
    {
        var source =
            "using System.Threading;\n" +
            "using System.Threading.Tasks;\n" +
            "using CulpeoStream.Generated;\n" +
            "[CulpeoStreamHandler]\n" +
            "public partial class NoInterfaceHandler { }\n";

        var (sources, diagnostics) = RunGenerator(source);
        Assert.NotEmpty(diagnostics.Where(d => d.Id == "CULPEO002"));
        Assert.DoesNotContain("NoInterfaceHandler_CulpeoGenerated.g.cs", sources.Keys);
    }

    [Fact]
    public void WrongFieldType_ProducesCULPEO003()
    {
        var source =
            Preamble +
            "[CulpeoStreamHandler]\n" +
            "public partial class WrongTypeHandler : ICulpeoStreamHandler\n" +
            "{\n" +
            "    [DeclareStream(\"audio\", CulpeoStreamType.Input, OffsetType.Byte, \"audio/opus\")]\n" +
            "    private string _audioIn = string.Empty;\n" +
            HandlerMembers +
            "}\n";

        var (_, diagnostics) = RunGenerator(source);
        Assert.NotEmpty(diagnostics.Where(d => d.Id == "CULPEO003"));
    }

    [Fact]
    public void SuffixCollision_ProducesCULPEO004_AndNoSource()
    {
        // F2: "audio-in" and "audio.in" both normalise to SafeMethodSuffix "AudioIn",
        // producing CS0111 if emitted. The generator must detect this and emit CULPEO004.
        var source =
            Preamble +
            "[CulpeoStreamHandler]\n" +
            "public partial class SuffixCollisionHandler : ICulpeoStreamHandler\n" +
            "{\n" +
            "    [DeclareStream(\"audio-in\", CulpeoStreamType.Input, OffsetType.Byte, \"audio/opus\")]\n" +
            "    private StreamDeclaration _audioIn = default!;\n" +
            "    [DeclareStream(\"audio.in\", CulpeoStreamType.Output, OffsetType.Byte, \"audio/opus\")]\n" +
            "    private StreamDeclaration _audioIn2 = default!;\n" +
            HandlerMembers +
            "}\n";

        var (sources, diagnostics) = RunGenerator(source);
        // Must produce CULPEO004 error
        Assert.NotEmpty(diagnostics.Where(d => d.Id == "CULPEO004"));
        // Must NOT emit the broken partial class (would produce CS0111)
        Assert.DoesNotContain("SuffixCollisionHandler_CulpeoGenerated.g.cs", sources.Keys);
    }

    // ── Tests: generated content quality ─────────────────────────────────────

    [Fact]
    public void GeneratedCode_DoesNotDispatchProtocolEvents_MiddlewareOwned()
    {
        // F9: protocol events (culpeo.*) are handled by middleware and must NOT
        // appear as switch arms in the generated OnMessageAsync dispatch.
        var source =
            Preamble +
            "[CulpeoStreamHandler]\n" +
            "public partial class FullHandler : ICulpeoStreamHandler\n" +
            "{\n" +
            HandlerMembers +
            "}\n";

        var (sources, _) = RunGenerator(source);
        var generated = sources["FullHandler_CulpeoGenerated.g.cs"];

        // Protocol event names must NOT be dispatch arms
        Assert.DoesNotContain("\"culpeo.init\"", generated);
        Assert.DoesNotContain("\"culpeo.ping\"", generated);
        Assert.DoesNotContain("\"culpeo.auth-refresh\"", generated);
        Assert.DoesNotContain("\"culpeo.close\"", generated);

        // Application event fallback must still be present
        Assert.Contains("OnUnknownEventAsync", generated);
        Assert.Contains("OnUnknownStreamAsync", generated);

        // Must contain the middleware explanation comment
        Assert.Contains("culpeo.*", generated);
        Assert.Contains("middleware", generated);
    }

    [Fact]
    public void GeneratedCode_ContainsAllDispatchInfrastructure()
    {
        var source =
            Preamble +
            "[CulpeoStreamHandler]\n" +
            "public partial class FullHandler : ICulpeoStreamHandler\n" +
            "{\n" +
            HandlerMembers +
            "}\n";

        var (sources, _) = RunGenerator(source);
        var generated = sources["FullHandler_CulpeoGenerated.g.cs"];

        Assert.Contains("RegisteredStreams", generated);
        Assert.Contains("OnMessageAsync", generated);
        Assert.Contains("HandleMediaAsync", generated);
        Assert.Contains("OnUnknownEventAsync", generated);
        Assert.Contains("OnUnknownStreamAsync", generated);
    }

    [Fact]
    public void GeneratedCode_IsNullableEnabled()
    {
        var source =
            Preamble +
            "[CulpeoStreamHandler]\n" +
            "public partial class NullableHandler : ICulpeoStreamHandler\n" +
            "{\n" +
            HandlerMembers +
            "}\n";

        var (sources, _) = RunGenerator(source);
        var generated = sources["NullableHandler_CulpeoGenerated.g.cs"];
        Assert.Contains("#nullable enable", generated);
    }

    [Fact]
    public void GeneratedCode_ContainsAutoGeneratedComment()
    {
        var source =
            Preamble +
            "[CulpeoStreamHandler]\n" +
            "public partial class CommentHandler : ICulpeoStreamHandler\n" +
            "{\n" +
            HandlerMembers +
            "}\n";

        var (sources, _) = RunGenerator(source);
        var generated = sources["CommentHandler_CulpeoGenerated.g.cs"];
        Assert.Contains("<auto-generated/>", generated);
    }
}
