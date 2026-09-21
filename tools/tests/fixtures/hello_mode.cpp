// A minimal mode for test_pack_opm.py. It declares the six required exports and one
// Bool param, so the packer has both a happy path and a param table to work on.
//
// pack_opm.py looks up unmangled names, so every export lives inside extern "C" for
// the compiler to emit bare names.
//
// A `const` at file scope has internal linkage by default, even inside extern
// "C" { }, and --gc-sections then prunes the param-table rodata that pack_opm.py
// needs to find kParams. A forward `extern "C" const T x;` before the definition
// gives it external linkage and keeps it.

#include <cstdint>

// This fixture stands alone, with no include of <operator_sdk.h> and its generated
// config. The ABI types it needs are forward-declared here, so pack_opm.py can read
// the ELF symbols without the fixture calling any SDK function.

struct OperatorApi;       // opaque; we don't call into it from a fixture
struct OpMidiMessage {    // layout-compatible with operator_sdk/abi/mode_api.h
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t port;
    uint8_t length;
};

// The ParamSpec layout, byte-for-byte: 32 bytes packed, the watcher in byte 28 and
// a 3-byte reserved tail. The tail is here only for the size cross-check.
struct __attribute__((packed)) ParamSpec {
    uint16_t name_offset;
    uint16_t options_offset;
    uint8_t  kind;
    uint8_t  is_per_instance;
    uint8_t  decimal_places;
    uint8_t  option_count;
    int32_t  min_value;
    int32_t  max_value;
    int32_t  default_value;
    int32_t  step_value;
    uint8_t  value_slot_count;
    uint8_t  _reserved[7];
};
static_assert(sizeof(ParamSpec) == 32, "ParamSpec must be 32 bytes");

// Forward declarations: promote the definitions below to external linkage.
extern "C" const ParamSpec kParams[1];
extern "C" const uint8_t   kParamCount;
extern "C" const char      kParamStrings[];

extern "C" {

// One Bool param, a minimal valid kParams so the packer has something
// to point the param table at.
const ParamSpec kParams[1] = {
    { /*name_offset*/ 0, /*options_offset*/ 0,
      /*kind*/ 0 /* ParamKind::Bool */,
      /*is_per_instance*/ 0,
      /*decimal_places*/ 0,
      /*option_count*/ 0,
      /*min_value*/ 0,
      /*max_value*/ 1,
      /*default_value*/ 0,
      /*step_value*/ 1,
      /*value_slot_count*/ 1,
      /*_reserved*/ { 0, 0, 0, 0 } }
};
const uint8_t kParamCount = 1;
const char    kParamStrings[] = "enable\0";

void mode_init(const OperatorApi* /*api*/) {}
void mode_process(OpMidiMessage* /*msgs*/, uint8_t /*count*/, uint32_t /*tick*/) {}
void mode_destroy() {}

}  // extern "C"
