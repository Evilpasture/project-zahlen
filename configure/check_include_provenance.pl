#!/usr/bin/env perl
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
#
# check_include_provenance.pl -- reject symbols used without the header that
# declares them.
#
# Perl port of the Python original this file replaces: same rule, same tables,
# same output, byte for byte -- cmake/GovernanceChecks.cmake runs this one at
# configure time with a find_program'd perl. The check is pure text processing,
# which is Perl's home ground; one process scans the whole tree in well under
# a second, and unlike a compiled checker there is nothing to build before the
# configure can be told a boundary was crossed. Reruns on an unchanged tree
# are faster still: per-file scan results are cached under the system temp
# directory, keyed by path, mtime and size, and ZHLN_PROVENANCE_CACHE=0 turns
# the cache off.
#
# The rule, from the consumer's side: for every symbol in the tables below,
# the file that names it must reach a provider through its own include closure
# -- directly, or through a header it includes, transitively. Reachability is
# computed here, not compiled, which is why this runs at CMake configure time
# and covers the whole tree in one pass. Three kinds of provider:
#
#   * first-party headers -- a repository path, reached through the closure;
#   * third-party headers -- matched by spelling (Jolt/), since the vendored
#     trees are submodules a checkout may not have;
#   * standard headers -- matched by spelling (span, string).
#
# Two more rules keep the graph honest. An include that names a first-party
# header and does not resolve is an error, not a third-party include (that is
# how a stale `#include "Types.hpp"` is caught after the header it named is
# gone -- RETIRED_HEADERS remembers the deleted names). And a tree that is
# umbrella-dependent on purpose (src/vulkan) is skipped, because its leaf
# headers are not meant to stand alone.
#
# The first Python version of this check did the obvious thing everywhere and
# took twenty seconds, which is the kind of configure step people learn to
# skip. Keep this shape: one walk of the tree, one read and one scan per file,
# one lookup per include, closures memoized -- and measure before keeping a
# slower one.

use strict;
use warnings;
use Storable qw();
use File::Spec ();
use Digest::SHA ();
use sort 'stable';
use Cwd qw(abs_path);
use File::Basename qw(basename dirname);

my $ROOT = dirname(dirname(abs_path($0)));

my @SOURCE_ROOTS = qw(include src plugins extensions gameplay modules tools tests samples app);
my %SOURCE_SUFFIXES = map { $_ => 1 }
    qw(.h .hh .hpp .hxx .inl .ipp .c .cc .cpp .cxx .ixx .cppm);
my %HEADER_SUFFIXES = map { $_ => 1 } qw(.h .hh .hpp .hxx .inl .ipp);
my %SKIP_DIR_NAMES = map { $_ => 1 }
    qw(.git build __pycache__ node_modules .cache);

# --- Provider table ------------------------------------------------------------------
#
# symbol -> providers. A first-party provider is a repository path; a THIRD_PARTY
# entry is a spelling pattern; a STANDARD entry is a header name without its
# angle brackets. The table deliberately names the symbols whose provenance this
# tree has already lost once -- add to it when a symbol starts being reached by
# accident, which is the moment the rule needs to exist.

my %FIRST_PARTY = (
    # Core
    AssetID          => ['include/Zahlen/Core/AssetID.hpp'],
    MaterialID       => ['include/Zahlen/Core/AssetID.hpp'],
    InvalidAssetID   => ['include/Zahlen/Core/AssetID.hpp'],
    InvalidMaterialID => ['include/Zahlen/Core/AssetID.hpp'],
    HashAssetID      => ['include/Zahlen/Core/AssetID.hpp'],
    EnableEnumFlags  => ['include/Zahlen/Core/EnumFlags.hpp'],
    EnumFlag         => ['include/Zahlen/Core/EnumFlags.hpp'],
    # Geometry
    Extent2D         => ['include/Zahlen/Geometry2D.hpp'],
    Offset2D         => ['include/Zahlen/Geometry2D.hpp'],
    ScissorRect      => ['include/Zahlen/Geometry2D.hpp'],
    ViewportRect     => ['include/Zahlen/Geometry2D.hpp'],
    # Vertex stream
    Packed1010102    => ['include/Zahlen/Vertex.hpp'],
    PackedHalf2      => ['include/Zahlen/Vertex.hpp'],
    PackedRGBA8      => ['include/Zahlen/Vertex.hpp'],
    VertexPosition     => ['include/Zahlen/Vertex.hpp'],
    VertexTangentFrame => ['include/Zahlen/Vertex.hpp'],
    VertexSurface      => ['include/Zahlen/Vertex.hpp'],
    VertexSkin         => ['include/Zahlen/Vertex.hpp'],
    # Audio
    AudioHandle      => ['include/Zahlen/Audio/AudioTypes.hpp'],
    SynthHandle      => ['include/Zahlen/Audio/AudioTypes.hpp'],
    AudioWaveformType => ['include/Zahlen/Audio/AudioTypes.hpp'],
    AudioFilterType  => ['include/Zahlen/Audio/AudioTypes.hpp'],
    AudioNoiseType   => ['include/Zahlen/Audio/AudioTypes.hpp'],
    # GUI
    UIBatch          => ['include/Zahlen/gui/UIData.hpp'],
    UIDrawData       => ['include/Zahlen/gui/UIData.hpp'],
    GlyphMetric      => ['include/Zahlen/gui/Font.hpp'],
    FontAtlas        => ['include/Zahlen/gui/Font.hpp'],
    # Renderer vocabulary
    TextureHandle    => ['include/Zahlen/Render/Handles.hpp'],
    BufferHandle     => ['include/Zahlen/Render/Handles.hpp'],
    PipelineHandle   => ['include/Zahlen/Render/Handles.hpp'],
    ResourceGroupHandle => ['include/Zahlen/Render/Handles.hpp'],
    SystemTextures   => ['include/Zahlen/Render/Handles.hpp'],
    RenderTextureHandle => ['include/Zahlen/Render/Handles.hpp'],
    FrameTarget      => ['include/Zahlen/Render/Handles.hpp'],
    Mesh             => ['include/Zahlen/Render/Types.hpp'],
    Material         => ['include/Zahlen/Render/Types.hpp'],
    DrawFlags        => ['include/Zahlen/Render/Types.hpp'],
    GPUVolumetricVolume => ['include/Zahlen/Render/Types.hpp'],
    CSGOperation     => ['include/Zahlen/Render/Types.hpp'],
    CSGModifier      => ['include/Zahlen/Render/Types.hpp'],
    # Meshlet contract
    GPUMeshlet       => ['include/Zahlen/Meshlet.hpp'],
    MeshletBuildResult => ['include/Zahlen/Meshlet.hpp'],
    kMeshletMaxVertices => ['include/Zahlen/Meshlet.hpp'],
    kMeshletMaxTriangles => ['include/Zahlen/Meshlet.hpp'],
    kMeshletConeWeight => ['include/Zahlen/Meshlet.hpp'],
    kMeshletsPerTaskGroup => ['include/Zahlen/Meshlet.hpp'],
    kMeshShaderGroupSize => ['include/Zahlen/Meshlet.hpp'],
    # Reached by accident before the sweep: the ECS spells LightType, the
    # renderer spells GraphicsSettings, and neither included its own header.
    LightType        => ['include/Zahlen/Render/GpuEnums.hpp'],
    ParticleAlignment => ['include/Zahlen/Render/GpuEnums.hpp'],
    QualityLevel     => ['include/Zahlen/GraphicsSettings.hpp'],
    AAMode           => ['include/Zahlen/GraphicsSettings.hpp'],
    AAState          => ['include/Zahlen/GraphicsSettings.hpp'],
    GISettings       => ['include/Zahlen/GraphicsSettings.hpp'],
    ShadowSettings   => ['include/Zahlen/GraphicsSettings.hpp'],
    RayTracingConfig => ['include/Zahlen/GraphicsSettings.hpp'],
    EnvironmentSettings => ['include/Zahlen/GraphicsSettings.hpp'],
    GraphicsSettings => ['include/Zahlen/GraphicsSettings.hpp'],
    FunctionRef      => ['include/Zahlen/Core/FunctionRef.hpp'],
    # Qualified: renderer builders already have unrelated Optional() members.
    'ZHLN::Optional' => ['include/Zahlen/Core/Optional.hpp'],
    Hash64           => ['include/Zahlen/Core/Hash.hpp'],
    Hash32           => ['include/Zahlen/Core/Hash.hpp'],
    HashCombine      => ['include/Zahlen/Core/Hash.hpp'],
);

my %THIRD_PARTY = (
    'JPH::' => '(^|/)Jolt/',
);
my %compiled_third_party;

my %STANDARD = (
    'std::span'        => 'span',
    'std::string_view' => 'string_view',
    'std::array'       => 'array',
    'std::optional'    => 'optional',
    'std::unique_ptr'  => 'memory',
    'std::make_unique' => 'memory',
    'std::vector'      => 'vector',
    'std::string'      => 'string',
);

# A file that re-exports instead of using: it is not compiled on its own and
# its own include list is not the thing under test.
my %SKIP_PATHS = map { $_ => 1 } ('modules/zahlen.cppm');

# Trees whose headers are umbrella-dependent on purpose: src/vulkan's leaf
# headers carry a #error demanding the umbrella, include nothing themselves,
# and are only ever compiled underneath it. Their vocabulary is local to that
# layer anyway -- Vk's TextureHandle is a heap handle and Vk's Extent2D() is a
# method, neither of them the public engine types this table is about.
my @SKIP_PREFIXES = ('src/vulkan/');

# Headers this tree removed on purpose, by basename. A deleted header cannot
# be found by looking for the names that exist, so a leftover
# `#include "Types.hpp"` resolves nowhere and is indistinguishable from an
# external header by spelling alone.
my %RETIRED_HEADERS = map { $_ => 1 } ('Types.hpp');

# Every tracked name in one alternation, so a file is scanned once for all of
# them rather than once per symbol. Longest first: with word boundaries a
# shorter name cannot match inside a longer one, but leftmost-first
# alternation would still let the short one win and hide the long one.
my @word_symbols = sort { length($b) <=> length($a) }
    grep { !/::$/ } (keys(%FIRST_PARTY), keys(%STANDARD), keys(%THIRD_PARTY));
my $WORD_SYMBOL_RE = do {
    my $alternation = join '|', map { quotemeta } @word_symbols;
    qr/(?<sym>\b(?:$alternation)\b)/;
};

my @prefix_symbols = grep { /::$/ } keys %THIRD_PARTY;

# A file that defines a name itself is not reaching for someone else's:
# `using TextureHandle = HeapHandle<...>` and `auto Extent2D() const` are
# locals. <name> is named the same in both branches, so $+{name} works for
# either; the keyword shapes share one leading alternation, which the regex
# engine can trie-dispatch (a (?|) branch reset defeats that and cost real
# time on every word boundary of every body).
my $SELF_DEFINITION_RE = qr/\b(?:using|typedef|struct|class|union|enum\s+class)\s+(?<name>[A-Za-z_]\w*)|\b(?:constexpr\s+)?auto\s+(?<name>[A-Za-z_]\w*)\s*\(/;
# /m is baked into the qr on purpose: a qr without it re-disables the
# match operator's /m inside its (?^:...) block, so ^ would only match at
# string start and every file after its first line loses its includes.
my $INCLUDE_RE = qr/^\s*#\s*include\s*([<"])([^>"]+)[>"]/m;
# One ordered scan over everything that is not code. The order *is* the point:
# at any position the first alternative that matches wins, so a // inside a
# string literal belongs to that literal and a " inside a comment belongs to
# the comment. Stripping comments first and literals second -- the obvious way
# round -- gets this wrong: it truncates `out.Line("a // b")` at the // and
# leaves an unterminated literal behind, which is exactly how a generated-code
# string in tools/zshader became a phantom std::span dependency.
# Deliberately NOT formatted across lines: this regex is not /x, so embedded
# whitespace would be literal pattern characters and silently match nothing.
my $SCAN_RE = qr{(?<include>^\s*#\s*include\s*[<"][^>"]+[>"])|(?<raw>R"(?<delim>[^()\s]{0,16})\(.*?\k<delim>")|(?<block>/\*.*?\*/)|(?<line>//[^\n]*)|(?<str>"(?:\\.|[^"\\\n])*")|(?<chr>'(?:\\.|[^'\\\n])*')}ms;
# One ordered scan over each file's raw text, same branch order the Python
# original uses: at any position the first alternative that matches wins, so
# a // inside a string literal belongs to that literal and a " inside a
# comment belongs to the comment. Stripping comments first and literals
# second -- the obvious way round -- gets this wrong: it truncates
# `out.Line("a // b")` at the // and leaves an unterminated literal behind,
# which is exactly how a generated-code string in tools/zshader became a
# phantom std::span dependency.
#
# Includes are collected straight out of the scan (their matched text is the
# directive itself), so there is no separate "keep" view. The inert view --
# comments and literals blanked, code and #include lines kept -- is built
# only for files whose body is inspected for symbol use; transitively closed
# headers (a third-party tree can be thousands of files) skip that work.
# strings are never kept in either: a header's *text* is not a header --
# tools/zshader/Emit.cpp carries the text of the files it generates inside
# R"ZHLN(...)" literals, and the #include in there is a line of generated
# output, not a dependency of Emit.cpp.
my $DIRECTIVE_RE = qr/^\s*#\s*include\s*([<"])([^>"]+)[>"]/;

# The scan that feeds the checks, one ordered pass per file: include
# directives are collected, and the two text views the checks need are built
# alongside -- keep (comments and literals blanked, #include lines kept; the
# dangling-include scan reads this, and its newlines are preserved so
# reported line numbers are the real ones) and inert (non-code blanked
# including #include lines; the symbol-use scans read this, so a name inside
# an include spelling is not a use of the symbol). Only files whose own body
# is inspected need the views; headers pulled in transitively skip them.
sub scan_all {
    my ($text) = @_;
    my (@found, $keep, $inert);
    $keep = $inert = '';
    my $pos = 0;
    while ($text =~ /$SCAN_RE/g) {
        my ($ms, $me) = ($-[0], $+[0]);
        my $matched = substr($text, $ms, $me - $ms);
        # %+ belongs to the LAST regex executed -- read the branch before
        # the directive parse below resets it.
        my $is_include = exists $+{include};
        if ($is_include) {
            push @found, [$1, $2] if $matched =~ /$DIRECTIVE_RE/;
        }
        my $gap = substr($text, $pos, $ms - $pos);
        $keep  .= $gap;
        $inert .= $gap;
        $pos = $me;
        my $newlines = $matched =~ tr/\n//;
        my $blank = "\n" x $newlines;
        if ($is_include) {
            $keep  .= $matched;
            $inert .= $blank;
        } else {
            $keep  .= $blank;
            $inert .= $blank;
        }
    }
    my $tail = substr($text, $pos);
    $keep  .= $tail;
    $inert .= $tail;
    return (\@found, $keep, $inert);
}

sub scan_includes {
    my ($text) = @_;
    my @found;
    while ($text =~ /$SCAN_RE/g) {
        next unless exists $+{include};
        my $matched = substr($text, $-[0], $+[0] - $-[0]);
        push @found, [$1, $2] if $matched =~ /$DIRECTIVE_RE/;
    }
    return \@found;
}

sub slurp {
    my ($path) = @_;
    open(my $fh, '<:raw', $path) or return undef;
    local $/;
    my $text = <$fh>;
    close $fh;
    return $text;
}

# Lexical os.path.normpath for the absolute paths this check builds: collapse
# duplicate slashes, drop '.', pop '..' without ever climbing above /.
sub normpath {
    my ($p) = @_;
    my $absolute = ($p =~ s{^/+}{}) ? 1 : 0;
    my @out;
    for my $part (grep { length && $_ ne '.' } split(m{/+}, $p)) {
        if ($part eq '..') {
            if (@out && $out[-1] ne '..') { pop @out }
            elsif (!$absolute)            { push @out, '..' }
        } else {
            push @out, $part;
        }
    }
    return ($absolute ? '/' : '') . join('/', @out);
}

# Every file of the repository's own roots, keyed by repo-relative path,
# walked once. Resolution asks this index, never the filesystem per candidate;
# extern/ and third_party/ are deliberately not walked: they are submodules
# with tens of thousands of files, no rule here is about their contents, and
# a spelling that lands there still resolves -- see resolve(), which falls
# back to the filesystem once per distinct spelling.
my %tree_files;
{
    my %skip_walk;
    my $walk;
    $walk = sub {
        my ($abs, $rel) = @_;
        opendir(my $dh, $abs) or return;
        for my $name (sort readdir($dh)) {
            next if $name eq '.' || $name eq '..';
            next if $SKIP_DIR_NAMES{$name};
            my $a = "$abs/$name";
            my $r = $rel eq '' ? $name : "$rel/$name";
            if (-d $a) {
                next if -l $a;    # pathlib's rglob does not descend symlinks
                $walk->($a, $r);
            } elsif (-f _) {
                $tree_files{$r} = $a;
            }
        }
        closedir($dh);
    };
    for my $root (@SOURCE_ROOTS) {
        $walk->("$ROOT/$root", $root) if -d "$ROOT/$root";
    }
}

my %resolvable = map { $tree_files{$_} => 1 } keys %tree_files;

# For each walked directory, the names directly inside it -- plus a full
# listdir of the repository root and of every search root below, directories
# included. The cheap half of resolution: a root without a spelling's first
# component cannot produce that spelling, and a set lookup is one step instead
# of a candidate path per root.
my %entries;
for my $rel (keys %tree_files) {
    my $abs = $tree_files{$rel};
    (my $parent = $abs) =~ s{/[^/]+$}{};
    $parent = '/' if $parent eq '';
    my $name = basename($abs);
    $entries{$parent}{$name} = 1;
}
my @search_roots = ($ROOT, "$ROOT/include", "$ROOT/src");
for my $name (qw(src plugins extensions gameplay extern third_party tests modules tools samples app include)) {
    my $base = "$ROOT/$name";
    next unless -d $base;
    push @search_roots, $base;
    opendir(my $dh, $base) or next;
    for my $child (sort grep { !/^\.{1,2}$/ } readdir($dh)) {
        my $c = "$base/$child";
        next unless -d $c;
        push @search_roots, $c;
        opendir(my $dh2, $c) or next;
        for my $gc (sort grep { !/^\.{1,2}$/ } readdir($dh2)) {
            push @search_roots, "$c/$gc" if -d "$c/$gc";
        }
        closedir($dh2);
    }
    closedir($dh);
}
for my $root (@search_roots) {
    next unless -d $root;
    opendir(my $dh, $root) or next;
    for my $name (grep { !/^\.{1,2}$/ } readdir($dh)) {
        $entries{$root}{$name} = 1;
    }
    closedir($dh);
}

# Basenames of every header in include/ and src/: how a broken first-party
# include tells itself apart from a third-party one. A bare
# `#include "Types.hpp"` that resolves nowhere points at a header of ours that
# no longer exists, while `#include "vk_mem_alloc.h"` is simply external.
my %first_party_header_names;
for my $rel (keys %tree_files) {
    next unless $rel =~ m{^(include|src)/};
    my ($suffix) = $rel =~ /(\.[^.]*)$/;
    next unless defined $suffix && $HEADER_SUFFIXES{lc $suffix};
    $first_party_header_names{basename($rel)} = 1;
}

# Where a quoted include of $including may legitimately look: beside the
# including file, then up through the directories that own it, then the public
# tree and src/ itself, then the documented cross-subsystem seams -- each one
# a PRIVATE include path in CMake (src/render reaches src/window and src/vulkan
# for the presentation seam and the RHI, src/window reaches src/engine for the
# TTY backend, the engine keeps its systems in a second include directory).
# Searching every root in the repository would resolve
# `#include "Types.hpp"` against an unrelated subsystem's private header of
# the same name, which is how a broken include hides.
my %_quoted_roots_cache;
sub quoted_roots {
    my ($including) = @_;
    return @{ $_quoted_roots_cache{$including} }
        if $_quoted_roots_cache{$including};
    my @roots;
    my $current = $including =~ s{/[^/]+$}{}r;
    while (1) {
        push @roots, $current;
        last if $current eq $ROOT || $current eq '/';
        $current = $current =~ s{/[^/]+$}{}r;
        $current = '/' if $current eq '';
    }
    push @roots, "$ROOT/include", "$ROOT/src";
    (my $relative = $including) =~ s{^\Q$ROOT\E/?}{};
    for my $seam (
        ['src/engine/',        "$ROOT/src/engine/system"],
        ['src/engine/system/', "$ROOT/src/engine"],
        ['src/render/',        "$ROOT/src/window"],
        ['src/render/',        "$ROOT/src/vulkan"],
        ['src/render/',        "$ROOT/src/render/init"],
        ['src/window/',        "$ROOT/src/engine"],
    ) {
        my ($prefix, $extra) = @$seam;
        push @roots, $extra if $relative =~ /^\Q$prefix\E/ && -d $extra;
    }
    $_quoted_roots_cache{$including} = \@roots;
    return @roots;
}

sub stat_search {
    my ($roots, $spelling) = @_;
    my $first = (split('/', $spelling, 2))[0];
    my $unfiltered = $spelling =~ /\.\./ || $spelling =~ m{^/};
    for my $root (@$roots) {
        if (!$unfiltered) {
            my $names = $entries{$root};
            next unless $names && $names->{$first};
        }
        my $candidate = normpath("$root/$spelling");
        return $candidate if -f $candidate;
    }
    return undef;
}

sub index_search {
    my ($roots, $spelling) = @_;
    if ($spelling =~ m{^/} || $spelling =~ /\.\./) {
        return stat_search($roots, $spelling);
    }
    my $first = (split('/', $spelling, 2))[0];
    for my $root (@$roots) {
        my $names = $entries{$root};
        next unless $names && $names->{$first};
        my $candidate = normpath("$root/$spelling");
        return $candidate if $resolvable{$candidate};
    }
    # Nothing in the walked tree: either a vendored header or a spelling that
    # resolves nowhere at all -- both answered by the filesystem, once.
    return stat_search($roots, $spelling);
}

# The include graph, with the include paths a target would really have.
my (%includes, %resolved, %texts, %closure_cache, %angle_cache);

sub resolve {
    my ($including, $delimiter, $spelling) = @_;
    if ($delimiter eq '"') {
        return index_search([quoted_roots($including)], $spelling);
    }
    return $angle_cache{$spelling} if exists $angle_cache{$spelling};
    my $found = index_search(\@search_roots, $spelling);
    $angle_cache{$spelling} = $found;
    return $found;
}

# Per-file records double as the on-disk cache: configure reruns on a tree
# that rarely changed, and re-reading, re-scanning and re-deriving the facts
# for half a thousand files is most of this script's cost. A record is
# [mtime, size, includes, resolved, dangling, used, defined]; the first two
# gate reuse, the last three exist only for files the check inspects itself.
# Invalidation is stat-based (mtime+size, like make); anything stale is
# dropped at load and recomputed exactly as a cold run would. Point
# ZHLN_PROVENANCE_CACHE at a path to relocate the cache, or set it to 0 to
# disable caching entirely.
my %records;
my $CACHE_FILE;
my $DIRTY = 0;

sub cache_file {
    return undef if defined $ENV{ZHLN_PROVENANCE_CACHE}
        && $ENV{ZHLN_PROVENANCE_CACHE} =~ /^(?:0|off)$/i;
    my $base = defined $ENV{ZHLN_PROVENANCE_CACHE}
        ? $ENV{ZHLN_PROVENANCE_CACHE}
        : File::Spec->tmpdir() . '/zahlen-include-provenance-'
            . Digest::SHA::sha1_hex($ROOT) . '.cache';
    return $base;
}

sub load_cache {
    $CACHE_FILE = cache_file();
    return unless defined $CACHE_FILE && -f $CACHE_FILE;
    my $data = eval { Storable::retrieve($CACHE_FILE) };
    return unless ref $data eq 'HASH';
    for my $path (keys %$data) {
        my $rec = $data->{$path};
        my @st = stat $path;
        next unless @st && $rec->[0] == $st[9] && $rec->[1] == $st[7];
        $records{$path}   = $rec;
        $includes{$path}  = $rec->[2];
        $resolved{$path}  = $rec->[3];
    }
}

sub save_cache {
    return unless $DIRTY && defined $CACHE_FILE;
    my $tmp = "$CACHE_FILE.$$";
    eval {
        Storable::nstore(\%records, $tmp);
        rename $tmp, $CACHE_FILE;
        1;
    } or do {
        unlink $tmp;
    };
}

sub read_file {
    my ($path) = @_;
    return if exists $includes{$path};
    my @st = stat $path;
    my $text = slurp($path);
    my ($found, $resolved_list) = ([], []);
    if (defined $text) {
        $found = scan_includes($text);
        my @r;
        for my $inc (@$found) {
            my $r = resolve($path, $inc->[0], $inc->[1]);
            push @r, $r if defined $r;
        }
        $resolved_list = \@r;
    }
    $includes{$path} = $found;
    $resolved{$path} = $resolved_list;
    $records{$path} = [
        @st ? ($st[9], $st[7]) : (0, 0),
        $found, $resolved_list, undef, undef, undef,
    ];
    $DIRTY = 1;
    return;
}

# Dangling includes: an include that does not resolve is broken rather than
# third-party when it can only have meant this repository -- it spells the
# public tree, it is a bare name that is exactly the name of a first-party
# header, or it names one this tree retired.
sub derive_facts {
    my ($path, $relative, $keep, $inert) = @_;
    my @dangling;
    my $line_number = 0;
    for my $line (split(/\n/, $keep, -1)) {
        $line_number++;
        next unless index($line, 'include') >= 0;
        next unless $line =~ $INCLUDE_RE;
        my $spelling = $2;
        next if defined resolve($path, $1, $spelling);
        my ($base) = $spelling =~ m{([^/]+)$};
        if (   $spelling =~ /^Zahlen\//
            || $spelling =~ m{^include/}
            || $first_party_header_names{$base}
            || $RETIRED_HEADERS{$base}) {
            push @dangling, [$relative, $spelling, $line_number];
        }
    }
    my %used;
    $used{ $+{sym} } = 1 while $inert =~ /$WORD_SYMBOL_RE/g;
    for my $prefix (@prefix_symbols) {
        $used{$prefix} = 1 if index($inert, $prefix) >= 0;
    }
    my %defined;
    $defined{ $1 // $2 } = 1 while $inert =~ /$SELF_DEFINITION_RE/g;
    return (\@dangling, \%used, \%defined);
}

# The checked-file facts (dangling includes, used symbols, locally defined
# names), from the record when it is fresh and complete, from a full scan
# otherwise. $relative is the caller's ROOT-stripped spelling of $path.
# Iterative, not recursive: the first-party graph has cycles (a facade and the
# headers it re-exports include each other), and a recursive walk cannot
# terminate on one.
my %rel_of_cache;
sub rel_of {
    my $rel = $rel_of_cache{ $_[0] };
    if (!defined $rel) {
        ($rel = $_[0]) =~ s{^\Q$ROOT\E/}{};
        $rel_of_cache{ $_[0] } = $rel;
    }
    return $rel;
}

sub closure {
    my ($path) = @_;
    return $closure_cache{$path} if $closure_cache{$path};
    my %seen;
    my @pending = ($path);
    while (@pending) {
        my $current = pop @pending;
        next if $seen{$current};
        $seen{$current} = 1;
        read_file($current) unless exists $includes{$current};
        push @pending, @{ $resolved{$current} };
    }
    $closure_cache{$path} = \%seen;
    return \%seen;
}

sub spellings {
    my ($path) = @_;
    my %out;
    for my $reachable (keys %{ closure($path) }) {
        read_file($reachable);
        for my $inc (@{ $includes{$reachable} }) {
            $out{ $inc->[1] } = 1;
        }
    }
    return \%out;
}


sub source_files {
    my @files;
    for my $root (@SOURCE_ROOTS) {
        my $base = "$ROOT/$root";
        next unless -d $base;
        my $walk;
        $walk = sub {
            my ($abs) = @_;
            opendir(my $dh, $abs) or return;
            for my $name (sort readdir($dh)) {
                next if $name eq '.' || $name eq '..';
                next if $SKIP_DIR_NAMES{$name};
                my $a = "$abs/$name";
                if (-d $a) {
                    next if -l $a;
                    $walk->($a);
                } elsif (-f _) {
                    (my $suffix) = $a =~ /(\.[^.]*)$/;
                    next unless defined $suffix && $SOURCE_SUFFIXES{$suffix};
                    push @files, $a;
                }
            }
            closedir($dh);
        };
        $walk->($base);
    }
    # Path.sort order is component-wise, not string-wise: sort by parts.
    my @parts_of = map { [split('/', $_)] } @files;
    my @order = sort {
        my @a = @{ $parts_of[$a] };
        my @b = @{ $parts_of[$b] };
        for my $i (0 .. (@a < @b ? $#a : $#b)) {
            my $c = $a[$i] cmp $b[$i];
            return $c if $c;
        }
        @a <=> @b;
    } 0 .. $#files;
    my %seen;
    return map { $files[$_] } grep { !$seen{ $files[$_] }++ } @order;
}

# The checked-file facts (dangling includes, used symbols, locally defined
# names), from the record when it is fresh and complete, from a full scan
# otherwise. $relative is the caller's ROOT-stripped spelling of $path.
sub facts_for {
    my ($path, $relative) = @_;
    if (!exists $includes{$path}) {
        # first touch is the checked read itself: one slurp, one scan
        my @st = stat $path;
        my $text = slurp($path);
        my ($found, $resolved_list) = ([], []);
        my @facts = ([], {}, {});
        if (defined $text) {
            ($found, my $keep, my $inert) = scan_all($text);
            my @r;
            for my $inc (@$found) {
                my $r = resolve($path, $inc->[0], $inc->[1]);
                push @r, $r if defined $r;
            }
            $resolved_list = \@r;
            @facts = derive_facts($path, $relative, $keep, $inert);
        }
        $includes{$path} = $found;
        $resolved{$path} = $resolved_list;
        $DIRTY = 1;
        return $records{$path} = [
            @st ? ($st[9], $st[7]) : (0, 0),
            $found, $resolved_list, @facts,
        ];
    }
    my $rec = $records{$path};
    return $rec if defined $rec->[4];
    # already known to the graph (preloaded, or read transitively) but never
    # inspected: scan now for the facts alone
    my $text = slurp($path);
    my @facts = ([], {}, {});
    if (defined $text) {
        (undef, my $keep, my $inert) = scan_all($text);
        @facts = derive_facts($path, $relative, $keep, $inert);
    }
    @$rec[4, 5, 6] = @facts;
    $DIRTY = 1;
    return $rec;
}

load_cache();
my @sources = source_files();

my (@violations, @dangling);
for my $path (@sources) {
    (my $relative = $path) =~ s{^\Q$ROOT\E/}{};
    next if $SKIP_PATHS{$relative};
    next if grep { $relative =~ /^\Q$_\E/ } @SKIP_PREFIXES;

    my $rec = facts_for($path, $relative);
    push @dangling, @{ $rec->[4] };
    my $used    = $rec->[5];
    my $defined = $rec->[6];

    my %reached = map { rel_of($_) => 1 } keys %{ closure($path) };
    my $external = spellings($path);

    # An include that does not resolve is broken rather than third-party when
    # it can only have meant this repository: it spells the public tree, it is
    # a bare name that is exactly the name of a first-party header, or it
    # names one this tree retired. (Computed inside facts_for, cached there.)

    for my $symbol (sort { $a cmp $b } keys %$used) {
        next if $defined->{$symbol};
        my $providers = $FIRST_PARTY{$symbol};
        if (defined $providers) {
            next if grep { $reached{$_} } @$providers;
            push @violations, [$relative, $symbol, join('/', @$providers)];
            next;
        }
        my $pattern = $THIRD_PARTY{$symbol};
        if (defined $pattern) {
            my $re = $compiled_third_party{$pattern} //= qr/$pattern/;
            next if grep { $_ =~ $re } keys %$external;
            push @violations, [$relative, $symbol, "an include matching $pattern"];
            next;
        }
        my $header = $STANDARD{$symbol};
        if (defined $header) {
            next if $external->{$header};
            push @violations, [$relative, $symbol, "<$header>"];
        }
    }
}

save_cache();

if (@dangling) {
    print STDERR
        "Broken includes -- a first-party header is named but does not resolve:\n";
    print STDERR "  $_->[0]:$_->[2] <$_->[1]>\n" for @dangling;
}

if (@violations) {
    print STDERR "Symbols reached without the header that declares them:\n";
    my $current;
    for my $v (sort { $a->[0] cmp $b->[0]
                   || $a->[1] cmp $b->[1]
                   || $a->[2] cmp $b->[2] } @violations) {
        my ($relative, $symbol, $provider) = @$v;
        if (!defined($current) || $relative ne $current) {
            print STDERR "\n  $relative\n";
            $current = $relative;
        }
        print STDERR "    $symbol  ->  needs $provider\n";
    }
    print STDERR
        "\nName the header yourself: a file must reach every type it spells through its own includes, "
        . "not through whatever its includes happen to include.\n";
}

exit 1 if @violations || @dangling;

printf
    "Include provenance OK (%d source files, %d first-party symbols, %d third-party namespaces tracked).\n",
    scalar(@sources), scalar(keys %FIRST_PARTY), scalar(keys %THIRD_PARTY);
exit 0;
