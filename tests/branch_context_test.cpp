#include <catch2/catch_test_macros.hpp>
#include <rex/codegen/binary_view.h>
#include <rex/codegen/config.h>
#include <rex/codegen/test_support.h>
#include "codegen/builders/builder_context.h"
#include <dis-asm.h>
#include <ppc-inst.h>
#include <array>

using namespace rex::codegen;

TEST_CASE("Branch emission retains the enclosing function after embedded data",
          "[codegen][branch-context]") {
    constexpr uint32_t base = 0x82000000;
    std::array<uint8_t, 0x200> bytes{};
    TestModule module;
    module.Load(base, bytes.data(), bytes.size());
    auto binary = BinaryView::fromModule(module);
    RecompilerConfig config;
    FunctionGraph graph;
    auto* function = graph.addFunction(base, 0x200, FunctionAuthority::CONFIG);
    // An overlapping speculative function can be discovered in a jump table.
    graph.addFunction(base + 0x40, 0x20, FunctionAuthority::GAP_FILL);

    EmitContext emit{binary, config, graph};
    ppc_insn insn{};
    insn.operands[0] = 6;             // cr6
    insn.operands[1] = base + 0x180;  // branch target in enclosing function
    RecompilerLocalVariables locals{};
    CSRState csr = CSRState::Unknown;
    std::string output;
    BuilderContext context{output, emit, *function, insn, base + 0x100,
                           nullptr, locals, csr};

    SECTION("Instruction after the speculative function") {
        REQUIRE(graph.getFunctionContaining(context.base) == nullptr);
    }
    SECTION("Instruction overlapping the speculative function") {
        context.base = base + 0x50;
        REQUIRE(graph.getFunctionContaining(context.base) != function);
    }
    context.emit_conditional_branch(false, "eq");
    CHECK(output.find("if (ctx.cr6.eq) goto loc_82000180;") != std::string::npos);
    CHECK(output.find("REX_FATAL") == std::string::npos);
}
