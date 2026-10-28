// Models.cs — M2-02: value-equatable pipeline models for the system generator.
//
// Every type here is a sealed record / ImmutableArray snapshot: the
// incremental pipeline caches by value, so NOTHING here may hold
// ISymbol, SyntaxNode, or Compilation references. Stage 1 snapshots syntax
// text; stage 2 resolves symbols and freezes plain strings.
using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Linq;
using System.Threading;

namespace Engine.Analyzers;

/// <summary>Ref-kind of one Tick parameter (matches C# syntax spelling).</summary>
internal enum TickRefKind
{
    /// <summary>By value (e.g. <c>float dt</c>). Copied per row.</summary>
    None = 0,

    /// <summary>Read-write component reference (<c>ref T</c>).</summary>
    Ref = 1,

    /// <summary>Read-only component reference (<c>in T</c>).</summary>
    In = 2,
}

/// <summary>One Tick parameter: ref-kind, fully-qualified type name, name.</summary>
internal sealed record TickParameterSnapshot(
    TickRefKind RefKind,
    string TypeName,
    string Name);

/// <summary>One user Tick overload: parameters + UpdateInPhase phases.</summary>
internal sealed record TickSignatureSnapshot(
    ImmutableArray<TickParameterSnapshot> Parameters,
    ImmutableArray<string> PhaseNames,
    TickRefKind SelfRefKind);

/// <summary>
/// One containing (outer) type redeclaration: keyword, name, type args.
/// The emitter re-opens each outer with its own keyword
/// (<c>struct</c>/<c>class</c>/<c>record struct</c>/<c>record class</c>) and
/// type parameters, omitting accessibility (legal in partial redeclarations).
/// </summary>
internal sealed record ContainingTypeSnapshot(
    string Keyword,
    string Name,
    string TypeArgs);

/// <summary>One system struct: identity + resolved Tick overloads.</summary>
internal sealed record SystemSpec(
    string Namespace,
    string StructName,
    ImmutableArray<ContainingTypeSnapshot> ContainingTypes,
    ImmutableArray<TickSignatureSnapshot> Ticks,
    ImmutableArray<string> StructPhaseNames,
    bool IsPublic);
