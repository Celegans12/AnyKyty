#include "common/fallback.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shaderCompiler.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace Libs::Graphics;
using Outcome = PipelineCache::RegisteredOutcome;
namespace Recompiler = Libs::Graphics::ShaderRecompiler;
namespace IR         = Recompiler::IR;

constexpr uint32_t ShiftLane     = 0x34020082u;
constexpr uint32_t LoadFormatX[] = {0xe0001000u, 0x80000201u};
constexpr uint32_t WaitAll       = 0xbf8c0000u;
constexpr uint32_t StoreRaw[]    = {0xe0701000u, 0x80010201u};
constexpr uint32_t EndProgram    = 0xbf810000u;
constexpr uint32_t Format32UInt  = 20;
constexpr uint32_t SwizzleXyzw   = 0xfacu;
constexpr uint32_t Records       = 128u;
constexpr uint32_t UserSgprs     = 8u;
constexpr uint32_t DispatchBase  = 0x41u;
constexpr uint32_t Wave32        = 1u << Pm4::COMPUTE_DISPATCH_INITIATOR_CS_W32_EN_SHIFT;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
	if (condition) return;
	std::fprintf(stderr, "FAILED: %s\n", what.c_str());
	g_failures++;
}

alignas(256) std::array<uint32_t, 8> g_compute {ShiftLane, LoadFormatX[0], LoadFormatX[1], WaitAll,
                                                StoreRaw[0], StoreRaw[1], EndProgram};
alignas(256) std::array<uint32_t, 8> g_second {ShiftLane, LoadFormatX[0], LoadFormatX[1], WaitAll,
                                               StoreRaw[0], StoreRaw[1], EndProgram};
alignas(256) std::array<uint32_t, 8> g_unknown {EndProgram};
alignas(256) std::array<uint32_t, 8> g_bare {EndProgram};
alignas(256) std::array<uint32_t, 8> g_pixel {EndProgram};
alignas(256) std::array<uint32_t, 8> g_stops {0xf80008cfu, 0x03020100u, EndProgram};

ShaderUserData g_users {};

std::vector<ShaderRegister> Registers(uint64_t address) {
	const uint32_t rsrc1 = 0xc0u << Pm4::COMPUTE_PGM_RSRC1_FLOAT_MODE_SHIFT;
	const uint32_t rsrc2 = (UserSgprs << Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_SHIFT) |
	                       (1u << Pm4::COMPUTE_PGM_RSRC2_TGID_X_EN_SHIFT) |
	                       (2u << Pm4::COMPUTE_PGM_RSRC2_LDS_SIZE_SHIFT);
	return {{Pm4::COMPUTE_PGM_LO, static_cast<uint32_t>(address >> 8u)},
	        {Pm4::COMPUTE_PGM_HI, static_cast<uint32_t>(address >> 40u) & 0xffu},
	        {Pm4::COMPUTE_PGM_RSRC1, rsrc1},
	        {Pm4::COMPUTE_PGM_RSRC2, rsrc2},
	        {Pm4::COMPUTE_NUM_THREAD_X, 64u},
	        {Pm4::COMPUTE_NUM_THREAD_Y, 1u},
	        {Pm4::COMPUTE_NUM_THREAD_Z, 1u}};
}

uint64_t Register(std::span<uint32_t> code, Prospero::ShaderBinaryType type, std::span<const ShaderRegister> registers,
                  const ShaderSpecialRegs* specials) {
	const auto address = reinterpret_cast<uint64_t>(code.data());
	ShaderMappedData data {.type            = type,
	                       .user_data       = &g_users,
	                       .code_size_bytes = static_cast<uint32_t>(std::ranges::find(code, EndProgram) - code.begin() + 1u) * 4u};
	data.sh_registers = registers;
	data.specials     = specials;
	ShaderMapUserData(address, data);
	return address;
}

std::vector<uint64_t> g_heard;

void Listen(void*, uint64_t address) {
	g_heard.push_back(address);
}

ShaderParams Dispatched(uint64_t address, uint32_t initiator, ShaderComputeInputInfo& info,
                        std::span<const uint32_t> user_data) {
	HW::ComputeShaderInfo regs {};
	for (const auto& reg: Registers(address)) {
		Check(HW::ApplyComputeShaderRegister(regs.cs_regs, reg.offset, reg.value), "a header register is not a compute register");
	}
	regs.cs_regs.wave_size = Pm4::ComputeWaveSize(initiator);
	std::ranges::copy(user_data, regs.cs_user_sgpr.value);
	info.dispatch_thread_dimensions = (initiator & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	return PrepareProgram(regs, info);
}

std::vector<uint32_t> StaticKey(ShaderComputeInputInfo info) {
	info.host_subgroup_size = 64u;
	std::vector<uint32_t> key;
	BuildStageStaticKey(info, key);
	return key;
}

Recompiler::CompileOptions Options(const ShaderParams& params, const ShaderComputeInputInfo& info) {
	Recompiler::CompileOptions options {.wave_size = info.wave_size};
	options.stage              = ShaderType::Compute;
	options.input_info.compute = &info;
	options.shader_hash        = params.hash;
	options.dump_ir            = false;
	options.user_data          = std::span(params.user_data).first(params.user_data_count);
	return options;
}

std::array<uint32_t, 4> MakeSharp(uint64_t address) {
	return {static_cast<uint32_t>(address), static_cast<uint32_t>((address >> 32u) & 0xffffu), Records * 4u,
	        SwizzleXyzw | (Format32UInt << 12u) | (1u << 24u) | (3u << 28u)};
}

void Listener() {
	const auto first = Register(g_compute, Prospero::ShaderBinaryType::kCs, Registers(reinterpret_cast<uint64_t>(g_compute.data())),
	                            [] {
		                            static ShaderSpecialRegs specials {};
		                            specials.dispatch_modifier = Wave32;
		                            return &specials;
	                            }());
	ShaderSetComputeListener(Listen, nullptr);
	Check(g_heard == std::vector<uint64_t> {first}, "a compute program registered before the listener is not handed to it");
	g_heard.clear();

	ShaderSpecialRegs specials {};
	specials.dispatch_modifier = Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS;
	const auto second = Register(g_second, Prospero::ShaderBinaryType::kCs,
	                             Registers(reinterpret_cast<uint64_t>(g_second.data())), &specials);
	Check(g_heard == std::vector<uint64_t> {second}, "a compute program registered after the listener is not handed to it");
	g_heard.clear();

	auto unknown = Registers(reinterpret_cast<uint64_t>(g_unknown.data()));
	unknown.push_back({Pm4::COMPUTE_PGM_RSRC2 + 0x40u, 0u});
	Register(g_unknown, Prospero::ShaderBinaryType::kCs, unknown, &specials);
	Register(g_bare, Prospero::ShaderBinaryType::kCs, Registers(reinterpret_cast<uint64_t>(g_bare.data())), nullptr);
	Register(g_pixel, Prospero::ShaderBinaryType::kPs, Registers(reinterpret_cast<uint64_t>(g_pixel.data())), &specials);
	Check(g_heard.empty(), "a program whose registers are not all known compute registers is handed to the listener");
	ShaderParams           params;
	ShaderComputeInputInfo info {};
	Check(!PrepareRegisteredProgram(reinterpret_cast<uint64_t>(g_unknown.data()), params, info),
	      "a compute program with a register the command processor does not take is prepared at registration");
	Check(!PrepareRegisteredProgram(reinterpret_cast<uint64_t>(g_bare.data()), params, info),
	      "a compute program without its dispatch modifier is prepared at registration");
	Check(!PrepareRegisteredProgram(reinterpret_cast<uint64_t>(g_pixel.data()), params, info),
	      "a pixel program is prepared as a compute program at registration");
	ShaderSetComputeListener(nullptr, nullptr);
	Register(g_second, Prospero::ShaderBinaryType::kCs, Registers(reinterpret_cast<uint64_t>(g_second.data())), &specials);
	Check(g_heard.empty(), "a removed listener is still called");
}

void SameKey(uint64_t address, uint32_t modifier) {
	ShaderParams           registered_params;
	ShaderComputeInputInfo registered {};
	Check(PrepareRegisteredProgram(address, registered_params, registered), "a registered compute program is not prepared");
	std::array<uint32_t, UserSgprs> user_data {};
	std::ranges::copy(MakeSharp(0x200810000ull), user_data.begin());
	ShaderComputeInputInfo dispatched {};
	const auto dispatch_params = Dispatched(address, DispatchBase | modifier, dispatched, user_data);
	Check(registered_params.hash == dispatch_params.hash, "the registered program has another hash than the dispatched one");
	Check(registered_params.user_data_count == dispatch_params.user_data_count,
	      "the registered program has another user data count than the dispatched one");
	Check(registered_params.code.data() == dispatch_params.code.data() &&
	          registered_params.code.size() == dispatch_params.code.size(),
	      "the registered program has other code than the dispatched one");
	Check(StaticKey(registered) == StaticKey(dispatched),
	      "the registered program has another static key than a dispatch with its modifier");
	auto other_queue          = dispatched;
	other_queue.async_compute = true;
	Check(StaticKey(other_queue) != StaticKey(dispatched), "the queue of a dispatch is not part of its static key");

	registered.host_subgroup_size = dispatched.host_subgroup_size = 64u;
	const auto registered_options = Options(registered_params, registered);
	std::unique_ptr<Recompiler::TranslateResult> translated;
	if (!Common::Fallback::Containable()) {
		Check(!PipelineCache::TranslatesAtRegistration(), "programs are translated at registration where a recompiler stop cannot be caught");
		const auto source = Recompiler::PrepareShaderSource(registered_params.code, registered_options);
		translated = std::make_unique<Recompiler::TranslateResult>(Recompiler::TranslateProgram(source.decoded, registered_options));
	} else {
		Check(PipelineCache::TranslateRegistered(registered_params, registered, translated) == Outcome::Translated && translated != nullptr,
		      "a registered compute program is not translated");
	}
	auto       from_registration = std::move(*translated);
	const auto dispatch_options  = Options(dispatch_params, dispatched);
	auto       from_dispatch     = Recompiler::TranslateProgram(dispatch_params.code, dispatch_options);
	const auto plan              = IR::ExtractResourcePlan(from_dispatch.program);
	std::array<uint32_t, 16> sharps {};
	std::ranges::copy(MakeSharp(0x200810000ull), sharps.begin());
	std::ranges::copy(MakeSharp(0x200820000ull), sharps.begin() + 4);
	const IR::SrtRuntime       runtime {.user_data = sharps};
	IR::ResourceSnapshot       snapshot;
	IR::ResourceSpecialization specialization;
	Check(IR::MaterializeResources(plan, runtime, snapshot, specialization), "the test program's resources are not materialized");
	const auto registered_spirv = Recompiler::CompileProgram(std::move(from_registration), registered_options, specialization, 0).spirv;
	const auto dispatch_spirv   = Recompiler::CompileProgram(std::move(from_dispatch), dispatch_options, specialization, 0).spirv;
	Check(!dispatch_spirv.empty() && registered_spirv == dispatch_spirv,
	      "a translation made at registration compiles to other SPIR-V than the one made at the dispatch");
}

void Stops() {
	ShaderSpecialRegs specials {};
	const auto        address = Register(g_stops, Prospero::ShaderBinaryType::kCs,
	                                     Registers(reinterpret_cast<uint64_t>(g_stops.data())), &specials);
	ShaderParams           params;
	ShaderComputeInputInfo info {};
	Check(PrepareRegisteredProgram(address, params, info), "the stopping program is not prepared at registration");
	if (!Common::Fallback::Containable()) {
		Check(!PipelineCache::TranslatesAtRegistration(), "programs are translated at registration where a recompiler stop cannot be caught");
		return;
	}
	Check(PipelineCache::TranslatesAtRegistration(), "programs are not translated at registration where a recompiler stop is caught");
	std::unique_ptr<Recompiler::TranslateResult> translated;
	Check(PipelineCache::TranslateRegistered(params, info, translated) == Outcome::Stopped && translated == nullptr,
	      "a translation at registration that the recompiler stops on is not dropped");
}

}

extern "C" int RunKytyComputeRegistrationTest() {

	Listener();
	SameKey(reinterpret_cast<uint64_t>(g_compute.data()), Wave32);
	SameKey(reinterpret_cast<uint64_t>(g_second.data()), Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS);
	Stops();
	if (g_failures != 0) {
		std::fprintf(stderr, "kyty_shim_compute_registration: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("kyty_shim_compute_registration: ok\n");
	return 0;
}
