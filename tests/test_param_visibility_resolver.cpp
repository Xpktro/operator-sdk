// The param visibility resolver, doctest behavior suite.
//
// A param's `.watch` and `.parent` reference another param by the address of its
// declaration, and the resolver maps that reference to a slot by identity, not by
// the display name. This builds a table with several sections that each carry an
// "Enable" and a gated "Child" under the same names, and checks that every Child
// resolves to the Enable and the section that are its own, and not to the first that
// shares the name.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk/params.h>
#include <operator_sdk/abi/mode_param.h>

#include <cstdint>

namespace op_params = ::op::modes::op_params;

// ---------------------------------------------------------------------------
// A synthetic table of three sections that each carry a section, an Enable and a
// Child under the same three names, which is what puts the shared names in front of
// the resolver. The references are `inline constexpr` at file scope, so each has a
// constant-expression address the resolver keys on, the way a real mode declares
// them.
//
// The spec layout, one spec per section:
//   0  Grp A section
//   1  Grp A Enable   watches spec 2
//   2  Grp A Child    gated on spec 1, parent spec 0
//   3  Grp B section
//   4  Grp B Enable   watches spec 5
//   5  Grp B Child    gated on spec 4, parent spec 3
//   6  Grp C section
//   7  Grp C Enable   watches spec 8
//   8  Grp C Child    gated on spec 7, parent spec 6
// ---------------------------------------------------------------------------

inline constexpr op::params::Section GrpA {.name = "Grp"};
inline constexpr op::params::Bool EnA {.name = "Enable", .default_ = false, .parent = GrpA};
inline constexpr op::params::Numeric ChA {
    .name         = "Child",
    .min          = 0,
    .max          = 127,
    .default_     = 0,
    .parent       = GrpA,
    .visible_when = {.watch = EnA, .op = op::params::Op::Eq, .value = 1}
};

inline constexpr op::params::Section GrpB {.name = "Grp"};
inline constexpr op::params::Bool EnB {.name = "Enable", .default_ = false, .parent = GrpB};
inline constexpr op::params::Numeric ChB {
    .name         = "Child",
    .min          = 0,
    .max          = 127,
    .default_     = 0,
    .parent       = GrpB,
    .visible_when = {.watch = EnB, .op = op::params::Op::Eq, .value = 1}
};

inline constexpr op::params::Section GrpC {.name = "Grp"};
inline constexpr op::params::Bool EnC {.name = "Enable", .default_ = false, .parent = GrpC};
inline constexpr op::params::Numeric ChC {
    .name         = "Child",
    .min          = 0,
    .max          = 127,
    .default_     = 0,
    .parent       = GrpC,
    .visible_when = {.watch = EnC, .op = op::params::Op::Eq, .value = 1}
};

// Stage the table at compile time, the path OP_MODE_PARAMS drives.
inline constexpr auto kBundle = op::params::detail::stage(GrpA, EnA, ChA, GrpB, EnB, ChB, GrpC, EnC, ChC);

namespace {
// Per-section expected spec indices.
constexpr std::uint8_t kSecSpec[3]    = {0, 3, 6};  // each child's own section
constexpr std::uint8_t kEnableSpec[3] = {1, 4, 7};  // each child's own sibling Enable
constexpr std::uint8_t kChildSpec[3]  = {2, 5, 8};  // each child's spec
}  // namespace

TEST_CASE("shared-name collision: staged bundle has the expected 9-spec shape") {
    REQUIRE(kBundle.param_count == 9);
}

TEST_CASE("shared-name collision: each gated child watches its own sibling Enable") {
    for (std::uint8_t g = 0; g < 3; ++g) {
        const auto& child         = kBundle.specs[kChildSpec[g]];
        const std::uint8_t packed = child.visible_when_param_id;

        // The child must be gated (not always-visible / never).
        REQUIRE(packed != op::modes::kVisibleAlways);
        REQUIRE(packed != op::modes::kVisibleNever);

        // The watcher index lives in its own ParamSpec byte.
        // Each child watches the Enable that is its own sibling, so the resolved
        // watcher is the group's Enable and not the first that shares the name.
        const std::uint8_t wid = op::modes::unpack_watcher_id(child);
        INFO("group " << int(g) << " child watcher spec index = " << int(wid) << " (expected "
                      << int(kEnableSpec[g]) << ")");
        CHECK(wid == kEnableSpec[g]);

        // Op encoding survives intact.
        CHECK(op::modes::unpack_op_code(packed) == static_cast<std::uint8_t>(op::params::Op::Eq));
    }
}

TEST_CASE("shared-name collision: each child's parent resolves to its own section") {
    for (std::uint8_t g = 0; g < 3; ++g) {
        const auto& child = kBundle.specs[kChildSpec[g]];
        // Each child's parent is its own section, even though the three sections
        // share the name "Grp".
        INFO("group " << int(g) << " child parent spec index = " << int(child.parent_param_id)
                      << " (expected " << int(kSecSpec[g]) << ")");
        CHECK(child.parent_param_id == kSecSpec[g]);
    }
}

TEST_CASE("shared-name collision: each Enable's parent resolves to its own section") {
    constexpr std::uint8_t kEnParent[3] = {0, 3, 6};
    for (std::uint8_t g = 0; g < 3; ++g) {
        const auto& en = kBundle.specs[kEnableSpec[g]];
        INFO("group " << int(g) << " Enable parent spec index = " << int(en.parent_param_id) << " (expected "
                      << int(kEnParent[g]) << ")");
        CHECK(en.parent_param_id == kEnParent[g]);
    }
}

TEST_CASE("shared-name collision: an ungated watcher (the Enable itself) stays always-visible") {
    // An Enable carries no .visible_when, so it serializes as always-visible.
    for (std::uint8_t g = 0; g < 3; ++g) {
        const auto& en = kBundle.specs[kEnableSpec[g]];
        CHECK(en.visible_when_param_id == op::modes::kVisibleAlways);
    }
}

// ---------------------------------------------------------------------------
// A 36-spec table, 12 groups of section, Enable and child, so a watcher index runs
// past 31. The watcher index has a byte of its own, so the last group's child, whose
// Enable is at spec 34, resolves to 34. This also checks that identity holds at
// scale.
// ---------------------------------------------------------------------------

#define GRP(N)                                                                                               \
    inline constexpr op::params::Section Sec##N {.name = "Grp"};                                             \
    inline constexpr op::params::Bool En##N {.name = "Enable", .default_ = false, .parent = Sec##N};         \
    inline constexpr op::params::Numeric Ch##N {                                                             \
        .name         = "Child",                                                                             \
        .min          = 0,                                                                                   \
        .max          = 127,                                                                                 \
        .default_     = 0,                                                                                   \
        .parent       = Sec##N,                                                                              \
        .visible_when = {.watch = En##N, .op = op::params::Op::Eq, .value = 1} \
    };

GRP(0)
GRP(1)
GRP(2) GRP(3) GRP(4) GRP(5) GRP(6) GRP(7) GRP(8) GRP(9) GRP(10) GRP(11)
#undef GRP

    inline constexpr auto kBigBundle = op::params::detail::stage(Sec0,
                                                                 En0,
                                                                 Ch0,
                                                                 Sec1,
                                                                 En1,
                                                                 Ch1,
                                                                 Sec2,
                                                                 En2,
                                                                 Ch2,
                                                                 Sec3,
                                                                 En3,
                                                                 Ch3,
                                                                 Sec4,
                                                                 En4,
                                                                 Ch4,
                                                                 Sec5,
                                                                 En5,
                                                                 Ch5,
                                                                 Sec6,
                                                                 En6,
                                                                 Ch6,
                                                                 Sec7,
                                                                 En7,
                                                                 Ch7,
                                                                 Sec8,
                                                                 En8,
                                                                 Ch8,
                                                                 Sec9,
                                                                 En9,
                                                                 Ch9,
                                                                 Sec10,
                                                                 En10,
                                                                 Ch10,
                                                                 Sec11,
                                                                 En11,
                                                                 Ch11);

TEST_CASE("watcher index: 36-spec table stages and the last group hits index >= 32") {
    REQUIRE(kBigBundle.param_count == 36);
    // Last group: Section 33, Enable 34, Child 35.
    const auto& child = kBigBundle.specs[35];
    REQUIRE(child.visible_when_param_id != op::modes::kVisibleAlways);
    REQUIRE(child.visible_when_param_id != op::modes::kVisibleNever);
    const std::uint8_t wid = op::modes::unpack_watcher_id(child);
    CHECK(wid == 34);  // watcher index >= 32, own byte
    CHECK(wid >= 32);
    CHECK(child.parent_param_id == 33);  // own section
    // Op survives in byte 26 (op-only; the watcher index has its own byte).
    CHECK(op::modes::unpack_op_code(child.visible_when_param_id)
          == static_cast<std::uint8_t>(op::params::Op::Eq));
}

TEST_CASE("watcher index: per-sibling identity holds across all 12 groups") {
    for (std::uint8_t g = 0; g < 12; ++g) {
        const std::uint8_t child_spec  = static_cast<std::uint8_t>(g * 3 + 2);
        const std::uint8_t enable_spec = static_cast<std::uint8_t>(g * 3 + 1);
        const auto& child              = kBigBundle.specs[child_spec];
        // Each child watches the Enable that is its own sibling.
        CHECK(op::modes::unpack_watcher_id(child) == enable_spec);
    }
}
