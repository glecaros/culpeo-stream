using System.Collections.Generic;
using System.Collections.Immutable;
using System.Text;
using CulpeoStream.SourceGen.Models;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace CulpeoStream.SourceGen.Generators;

/// <summary>
/// Incremental source generator for <c>[CulpeoStreamHandler]</c> and <c>[DeclareStream]</c>.
///
/// <para>For each <c>partial</c> class annotated with <c>[CulpeoStreamHandler]</c>, the generator emits:</para>
/// <list type="bullet">
///   <item><c>RegisteredStreams</c> — static, reflection-free <c>IReadOnlyList&lt;StreamDeclaration&gt;</c>.</item>
///   <item><c>OnMessageAsync</c> — switch-expression dispatch over event name strings.</item>
///   <item><c>HandleMediaAsync</c> — switch-expression dispatch over stream IDs.</item>
///   <item>Protected virtual handler stubs for each event/stream.</item>
/// </list>
///
/// <para>Diagnostics produced:</para>
/// <list type="bullet">
///   <item>CULPEO001 — duplicate stream ID.</item>
///   <item>CULPEO002 — class does not implement <c>ICulpeoStreamHandler</c>.</item>
///   <item>CULPEO003 — <c>[DeclareStream]</c> applied to a field whose type is not <c>StreamDeclaration</c>.</item>
/// </list>
/// </summary>
[Generator]
public sealed class HandlerGenerator : IIncrementalGenerator
{
    // ── Attribute fully-qualified names ─────────────────────────────────────────

    private const string HandlerAttrFqn = "CulpeoStream.Generated.CulpeoStreamHandlerAttribute";
    // F7: use fully-qualified names to avoid false matches from user-defined types with the same simple name.
    private const string DeclareStreamAttrFqn = "CulpeoStream.Generated.DeclareStreamAttribute";
    private const string ICulpeoStreamHandlerFqn = "CulpeoStream.AspNetCore.ICulpeoStreamHandler";
    private const string StreamDeclarationFqn = "CulpeoStream.Core.StreamDeclaration";

    // ── Diagnostic descriptors ────────────────────────────────────────────────

    private static readonly DiagnosticDescriptor Culpeo001 = new(
        id: "CULPEO001",
        title: "Duplicate stream ID",
        messageFormat: "Stream ID '{0}' is declared more than once on handler '{1}'",
        category: "CulpeoStream",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor Culpeo002 = new(
        id: "CULPEO002",
        title: "Handler does not implement ICulpeoStreamHandler",
        messageFormat: "Class '{0}' is annotated with [CulpeoStreamHandler] but does not implement ICulpeoStreamHandler",
        category: "CulpeoStream",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor Culpeo003 = new(
        id: "CULPEO003",
        title: "[DeclareStream] applied to non-StreamDeclaration field",
        messageFormat: "Field '{0}' is annotated with [DeclareStream] but its type is not StreamDeclaration",
        category: "CulpeoStream",
        defaultSeverity: DiagnosticSeverity.Warning,
        isEnabledByDefault: true);

    // F2: method-suffix collision diagnostic
    private static readonly DiagnosticDescriptor Culpeo004 = new(
        id: "CULPEO004",
        title: "Stream IDs produce duplicate generated method name",
        messageFormat: "Stream IDs '{0}' and '{1}' produce the same generated method name '{2}'. Rename one to avoid a compile error.",
        category: "CulpeoStream",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    // ── IIncrementalGenerator implementation ──────────────────────────────────

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        // Step 1: Emit the attribute source code into every consuming compilation.
        context.RegisterPostInitializationOutput(static ctx =>
        {
            ctx.AddSource("CulpeoStreamAttributes.g.cs",
                SourceText.From(AttributeSource, Encoding.UTF8));
        });

        // Step 2: Collect all classes annotated with [CulpeoStreamHandler].
        var handlerCandidates = context.SyntaxProvider
            .ForAttributeWithMetadataName(
                HandlerAttrFqn,
                predicate: static (node, _) => node is ClassDeclarationSyntax,
                transform: static (ctx, ct) => TransformHandler(ctx, ct))
            .Where(static d => d is not null);

        // Step 3: Emit source and report diagnostics for each candidate.
        context.RegisterSourceOutput(handlerCandidates, static (spc, descriptor) =>
        {
            if (descriptor is null) return;

            // Report all captured diagnostics (errors + warnings).
            foreach (var diag in descriptor.Diagnostics)
            {
                var location = diag.FilePath is not null
                    ? Location.Create(diag.FilePath,
                        new Microsoft.CodeAnalysis.Text.TextSpan(0, 0),
                        new Microsoft.CodeAnalysis.Text.LinePositionSpan(
                            new Microsoft.CodeAnalysis.Text.LinePosition(diag.Line, diag.Column),
                            new Microsoft.CodeAnalysis.Text.LinePosition(diag.Line, diag.Column)))
                    : Location.None;

                DiagnosticDescriptor desc = diag.Id switch
                {
                    "CULPEO001" => Culpeo001,
                    "CULPEO002" => Culpeo002,
                    "CULPEO003" => Culpeo003,
                    "CULPEO004" => Culpeo004,
                    _ => Culpeo002
                };

                spc.ReportDiagnostic(Diagnostic.Create(desc, location, diag.MessageArgs));
            }

            // Do not emit source if there are errors.
            bool hasErrors = false;
            foreach (var d in descriptor.Diagnostics)
            {
                if (d.Severity == DiagnosticSeverity.Error) { hasErrors = true; break; }
            }
            if (hasErrors) return;

            var sourceText = CodeGenerator.Generate(descriptor);
            spc.AddSource(
                $"{descriptor.ClassName}_CulpeoGenerated.g.cs",
                SourceText.From(sourceText, Encoding.UTF8));
        });
    }

    // ── Transform ─────────────────────────────────────────────────────────────

    private static HandlerDescriptor? TransformHandler(
        GeneratorAttributeSyntaxContext ctx,
        System.Threading.CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();

        if (ctx.TargetSymbol is not INamedTypeSymbol symbol) return null;
        var classSyntax = (ClassDeclarationSyntax)ctx.TargetNode;

        var diagnostics = new List<DiagnosticInfo>();

        // ── Check partial ────────────────────────────────────────────────────
        bool isPartial = false;
        foreach (var mod in classSyntax.Modifiers)
        {
            if (mod.IsKind(SyntaxKind.PartialKeyword)) { isPartial = true; break; }
        }
        // Note: we don't block generation on non-partial — the compiler will raise CS0260.

        // ── Check ICulpeoStreamHandler ───────────────────────────────────────
        bool implementsHandler = false;
        foreach (var iface in symbol.AllInterfaces)
        {
            // F7: use fully-qualified name to avoid false match from user-defined ICulpeoStreamHandler
            if (iface.ToDisplayString() == ICulpeoStreamHandlerFqn) { implementsHandler = true; break; }
        }
        if (!implementsHandler)
        {
            var loc = GetLocation(classSyntax.Identifier.GetLocation());
            diagnostics.Add(new DiagnosticInfo(
                "CULPEO002",
                $"Class '{symbol.Name}' is annotated with [CulpeoStreamHandler] but does not implement ICulpeoStreamHandler",
                DiagnosticSeverity.Error,
                loc.FilePath, loc.Line, loc.Column,
                new object[] { symbol.Name }));
        }

        // ── Collect [DeclareStream] fields ───────────────────────────────────
        var streams = new List<StreamDescriptor>();
        var seenIds = new System.Collections.Generic.HashSet<string>(StringComparer.Ordinal);

        foreach (var member in symbol.GetMembers())
        {
            ct.ThrowIfCancellationRequested();
            if (member is not IFieldSymbol field) continue;

            AttributeData? declareAttr = null;
            foreach (var attr in field.GetAttributes())
            {
                // F7: use fully-qualified attribute name to avoid matching a user-defined DeclareStreamAttribute
                if (attr.AttributeClass?.ToDisplayString() == DeclareStreamAttrFqn)
                {
                    declareAttr = attr;
                    break;
                }
            }
            if (declareAttr is null) continue;

            // CULPEO003: field type must be StreamDeclaration (F7: use FQN)
            if (field.Type.ToDisplayString() != StreamDeclarationFqn)
            {
                var loc = GetLocation(member.Locations.IsEmpty ? Location.None : member.Locations[0]);
                diagnostics.Add(new DiagnosticInfo(
                    "CULPEO003",
                    $"Field '{field.Name}' is annotated with [DeclareStream] but its type is not StreamDeclaration",
                    DiagnosticSeverity.Warning,
                    loc.FilePath, loc.Line, loc.Column,
                    new object[] { field.Name }));
                continue;
            }

            var args = declareAttr.ConstructorArguments;
            if (args.Length < 4) continue;

            var id = args[0].Value as string ?? string.Empty;
            var streamTypeInt = args[1].Value is int st ? st : (args[1].Value is null ? 0 : Convert.ToInt32(args[1].Value));
            var offsetTypeInt = args[2].Value is int ot ? ot : (args[2].Value is null ? 0 : Convert.ToInt32(args[2].Value));
            var contentType = args[3].Value as string ?? string.Empty;
            var purpose = args.Length > 4 ? args[4].Value as string : null;

            // CULPEO001: duplicate stream ID
            if (!seenIds.Add(id))
            {
                var loc = GetLocation(member.Locations.IsEmpty ? Location.None : member.Locations[0]);
                diagnostics.Add(new DiagnosticInfo(
                    "CULPEO001",
                    $"Stream ID '{id}' is declared more than once on handler '{symbol.Name}'",
                    DiagnosticSeverity.Error,
                    loc.FilePath, loc.Line, loc.Column,
                    new object[] { id, symbol.Name }));
                continue;
            }

            var fieldLoc = GetLocation(member.Locations.IsEmpty ? Location.None : member.Locations[0]);
            streams.Add(new StreamDescriptor(
                id,
                contentType,
                streamTypeInt,
                offsetTypeInt,
                purpose,
                field.Name,
                fieldLoc.FilePath, fieldLoc.Line, fieldLoc.Column));
        }

        // ── F2: Check for SafeMethodSuffix collisions → CULPEO004 ───────────────
        var suffixToFirstStream = new System.Collections.Generic.Dictionary<string, StreamDescriptor>(StringComparer.Ordinal);
        foreach (var stream in streams)
        {
            var suffix = stream.SafeMethodSuffix;
            if (suffixToFirstStream.TryGetValue(suffix, out var first))
            {
                var loc = GetLocation(Location.None);
                diagnostics.Add(new DiagnosticInfo(
                    "CULPEO004",
                    $"Stream IDs '{first.Id}' and '{stream.Id}' produce the same generated method name '{suffix}'. Rename one to avoid a compile error.",
                    DiagnosticSeverity.Error,
                    loc.FilePath, loc.Line, loc.Column,
                    new object[] { first.Id, stream.Id, suffix }));
            }
            else
            {
                suffixToFirstStream[suffix] = stream;
            }
        }

        var ns = symbol.ContainingNamespace?.IsGlobalNamespace == true
            ? null
            : symbol.ContainingNamespace?.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat
                .WithGlobalNamespaceStyle(SymbolDisplayGlobalNamespaceStyle.Omitted));

        return new HandlerDescriptor(
            ns,
            symbol.Name,
            isPartial,
            implementsHandler,
            streams.ToImmutableArray(),
            diagnostics.ToImmutableArray());
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    private static (string? FilePath, int Line, int Column) GetLocation(Location location)
    {
        if (location == Location.None || location.Kind == LocationKind.None)
            return (null, 0, 0);

        var lineSpan = location.GetLineSpan();
        return (lineSpan.Path, lineSpan.StartLinePosition.Line, lineSpan.StartLinePosition.Character);
    }

    // ── Attribute source (emitted into user compilation) ─────────────────────

    /// <summary>
    /// Source code for the attributes emitted at post-initialization.
    /// References <c>global::CulpeoStream.Core</c> types so the consuming project
    /// must reference <c>CulpeoStream.Core</c> (it always does — it implements ICulpeoStreamHandler
    /// from CulpeoStream.AspNetCore which depends on Core).
    /// </summary>
    private const string AttributeSource = """
        // <auto-generated/>
        // CulpeoStream source generator attributes — do not modify.
        #nullable enable
        namespace CulpeoStream.Generated
        {
            /// <summary>
            /// Apply to a <see langword="partial"/> class implementing
            /// <c>ICulpeoStreamHandler</c> to generate reflection-free dispatch infrastructure.
            /// </summary>
            [global::System.AttributeUsage(
                global::System.AttributeTargets.Class,
                AllowMultiple = false,
                Inherited = false)]
            internal sealed class CulpeoStreamHandlerAttribute : global::System.Attribute { }

            /// <summary>
            /// Apply to a field of type <c>StreamDeclaration</c> to declare a stream
            /// in the generated <c>RegisteredStreams</c> list.
            /// </summary>
            [global::System.AttributeUsage(
                global::System.AttributeTargets.Field,
                AllowMultiple = false,
                Inherited = false)]
            internal sealed class DeclareStreamAttribute : global::System.Attribute
            {
                /// <param name="id">Stream identifier, e.g. <c>"audio-in"</c>.</param>
                /// <param name="streamType">Stream directionality.</param>
                /// <param name="offsetType">How the offset advances per frame.</param>
                /// <param name="contentType">MIME type, e.g. <c>"audio/pcm;rate=16000;channels=1;bits=16"</c>.</param>
                /// <param name="purpose">Optional semantic label (required when two streams share the same type).</param>
                public DeclareStreamAttribute(
                    string id,
                    global::CulpeoStream.Core.CulpeoStreamType streamType,
                    global::CulpeoStream.Core.OffsetType offsetType,
                    string contentType,
                    string? purpose = null)
                {
                    Id = id;
                    StreamType = streamType;
                    OffsetType = offsetType;
                    ContentType = contentType;
                    Purpose = purpose;
                }

                public string Id { get; }
                public global::CulpeoStream.Core.CulpeoStreamType StreamType { get; }
                public global::CulpeoStream.Core.OffsetType OffsetType { get; }
                public string ContentType { get; }
                public string? Purpose { get; }
            }
        }
        """;
}
