#include "graphics/guest_gpu/pm4.h"

#include "common/assert.h"
#include "common/file.h"
#include "graphics/guest_gpu/hardwareContext.h"

#include <array>

namespace Libs::Graphics::Pm4 {

namespace {

enum class PacketType : uint32_t { Type0, Type1, Type2, Type3 };

constexpr auto MakeRegisterNames() {
	std::array<const char*, R_NUM> names {};
	for (auto& name: names) {
		name = "<unknown>";
	}

	names[R_ZERO]           = "R_ZERO";
	names[R_DISPATCH_RESET] = "R_DISPATCH_RESET";
	names[R_PUSH_MARKER]    = "R_PUSH_MARKER";
	names[R_POP_MARKER]     = "R_POP_MARKER";
	names[R_CONTEXT_STATE]  = "R_CONTEXT_STATE";
	return names;
}

constexpr auto MakeOpcodeNames() {
	std::array<const char*, 256> names {};
	for (auto& name: names) {
		name = "<unknown>";
	}

	names[IT_NOP]                       = "IT_NOP";
	names[IT_SET_BASE]                  = "IT_SET_BASE";
	names[IT_CLEAR_STATE]               = "IT_CLEAR_STATE";
	names[IT_INDEX_BUFFER_SIZE]         = "IT_INDEX_BUFFER_SIZE";
	names[IT_DISPATCH_DIRECT]           = "IT_DISPATCH_DIRECT";
	names[IT_DISPATCH_INDIRECT]         = "IT_DISPATCH_INDIRECT";
	names[IT_SET_PREDICATION]           = "IT_SET_PREDICATION";
	names[IT_COND_EXEC]                 = "IT_COND_EXEC";
	names[IT_DRAW_INDIRECT]             = "IT_DRAW_INDIRECT";
	names[IT_DRAW_INDEX_INDIRECT]       = "IT_DRAW_INDEX_INDIRECT";
	names[IT_INDEX_BASE]                = "IT_INDEX_BASE";
	names[IT_DRAW_INDEX_2]              = "IT_DRAW_INDEX_2";
	names[IT_CONTEXT_CONTROL]           = "IT_CONTEXT_CONTROL";
	names[IT_INDEX_TYPE]                = "IT_INDEX_TYPE";
	names[IT_DRAW_INDIRECT_MULTI]       = "IT_DRAW_INDIRECT_MULTI";
	names[IT_DRAW_INDEX_AUTO]           = "IT_DRAW_INDEX_AUTO";
	names[IT_NUM_INSTANCES]             = "IT_NUM_INSTANCES";
	names[IT_DRAW_INDEX_OFFSET_2]       = "IT_DRAW_INDEX_OFFSET_2";
	names[IT_WRITE_DATA]                = "IT_WRITE_DATA";
	names[IT_MEM_SEMAPHORE]             = "IT_MEM_SEMAPHORE";
	names[IT_DRAW_INDEX_INDIRECT_MULTI] = "IT_DRAW_INDEX_INDIRECT_MULTI";
	names[IT_WAIT_REG_MEM]               = "IT_WAIT_REG_MEM";
	names[IT_INDIRECT_BUFFER]           = "IT_INDIRECT_BUFFER";
	names[IT_COPY_DATA]                 = "IT_COPY_DATA";
	names[IT_CP_DMA]                    = "IT_CP_DMA";
	names[IT_PFP_SYNC_ME]               = "IT_PFP_SYNC_ME";
	names[IT_SURFACE_SYNC]              = "IT_SURFACE_SYNC";
	names[IT_EVENT_WRITE]               = "IT_EVENT_WRITE";
	names[IT_RELEASE_MEM]               = "IT_RELEASE_MEM";
	names[IT_DMA_DATA]                  = "IT_DMA_DATA";
	names[IT_ACQUIRE_MEM]               = "IT_ACQUIRE_MEM";
	names[IT_REWIND]                    = "IT_REWIND";
	names[IT_SET_SH_REG_INDIRECT]       = "IT_SET_SH_REG_INDIRECT";
	names[IT_SET_UCONFIG_REG_INDIRECT]  = "IT_SET_UCONFIG_REG_INDIRECT";
	names[IT_SET_CONFIG_REG]            = "IT_SET_CONFIG_REG";
	names[IT_SET_CONTEXT_REG]           = "IT_SET_CONTEXT_REG";
	names[IT_SET_SH_REG]                = "IT_SET_SH_REG";
	names[IT_SET_QUEUE_REG]             = "IT_SET_QUEUE_REG";
	names[IT_SET_UCONFIG_REG]           = "IT_SET_UCONFIG_REG";
	names[IT_SET_UCONFIG_REG_INDEX]     = "IT_SET_UCONFIG_REG_INDEX";
	names[IT_DRAW_INDEX_MULTI_INST]     = "IT_DRAW_INDEX_MULTI_INST";
	names[IT_DISPATCH_DRAW]             = "IT_DISPATCH_DRAW";
	names[IT_GET_LOD_STATS]             = "IT_GET_LOD_STATS";
	names[IT_WAIT_REG_MEM_64]           = "IT_WAIT_REG_MEM_64";
	names[IT_SET_CONTEXT_REG_INDIRECT]  = "IT_SET_CONTEXT_REG_INDIRECT";
	return names;
}

constexpr auto g_register_names = MakeRegisterNames();
constexpr auto g_opcode_names   = MakeOpcodeNames();

}

void DumpPm4PacketStream(Common::File* file, const uint32_t* cmd_buffer, uint32_t start_dw,
                         uint32_t num_dw) {
	file->Printf("----- Buffer --- dwords: 0x%05" PRIx32 ", offset : %u, addr: %016" PRIx64
	             " ----- \n",
	             num_dw, start_dw, reinterpret_cast<uint64_t>(cmd_buffer));

	auto* cmd = cmd_buffer + start_dw;
	auto  dw  = num_dw;
	while (dw != 0) {
		EXIT_NOT_IMPLEMENTED(dw > num_dw);

		auto cmd_id = *cmd++;

		file->Printf("%05" PRIx32 " | 0x%08" PRIx32 " | ", start_dw, cmd_id);

		uint32_t len = 0;

		const auto packet_type = static_cast<PacketType>(cmd_id >> 30u);
		EXIT_NOT_IMPLEMENTED(dw < 2 && packet_type != PacketType::Type2 &&
		                     PacketSizeDw(cmd_id) != 1u);

		switch (packet_type) {
			case PacketType::Type3: {
				const bool    sh_gx = (cmd_id & 0x2u) == 0;
				const uint8_t op    = ((cmd_id >> 8u) & 0xffu);
				const auto    r     = KYTY_PM4_R(cmd_id);
				len                 = PacketSizeDw(cmd_id) - 1u;

				EXIT_NOT_IMPLEMENTED(len >= dw);

				file->Printf("%s %s(OP:0x%02" PRIx8 ") SH:%s CNT:%u\n", g_opcode_names[op],
				             (op == IT_NOP ? g_register_names[r] : ""), op, sh_gx ? "GX" : "CX",
				             len);

				for (uint32_t i = 0; i < len; i++) {
					file->Printf("      | 0x%08" PRIx32 " | \n", cmd[i]);
				}

				if ((op == IT_SET_CONTEXT_REG_INDIRECT || op == IT_SET_SH_REG_INDIRECT ||
				     op == IT_SET_UCONFIG_REG_INDIRECT) &&
				    len == 4) {
					auto* indirect_buffer = reinterpret_cast<const uint32_t*>(
					    (static_cast<uint64_t>(cmd[0]) & 0xfffffffcu) |
					    (static_cast<uint64_t>(cmd[1]) << 32u));
					const uint32_t        indirect_num_regs = cmd[3] & 0x3fffu;
					const LoadedRegisters registers {indirect_buffer, cmd[2]};

					for (uint32_t i = 0; indirect_buffer != nullptr && i < indirect_num_regs; i++) {
						file->Printf("      |            | offset = 0x%08" PRIx32
						             ", value = 0x%08" PRIx32 "\n",
						             registers.RawOffset(i), registers.Value(i));
					}
				}
				break;
			}
			case PacketType::Type0:
			case PacketType::Type1:
			case PacketType::Type2:
				file->Printf("<unsupported TYPE%u packet>\n", static_cast<uint32_t>(packet_type));
				break;
		}

		cmd += len;
		dw -= len + 1;
		start_dw += len + 1;
	}
}

}

namespace Libs::Graphics::HW {

bool ApplyComputeShaderRegister(CsStageRegisters& cs_regs, uint32_t cmd_offset, uint32_t value) {
	switch (cmd_offset) {
		case Pm4::COMPUTE_PGM_LO:
			cs_regs.data_addr &= 0xFFFFFF00000000FFull;
			cs_regs.data_addr |= static_cast<uint64_t>(value) << 8u;
			break;
		case Pm4::COMPUTE_PGM_HI:
			cs_regs.data_addr &= 0xFFFF00FFFFFFFFFFull;
			cs_regs.data_addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			break;
		case Pm4::COMPUTE_PGM_RSRC1:
			cs_regs.vgprs =
			    (value >> Pm4::COMPUTE_PGM_RSRC1_VGPRS_SHIFT) & Pm4::COMPUTE_PGM_RSRC1_VGPRS_MASK;
			cs_regs.priority = (value >> Pm4::COMPUTE_PGM_RSRC1_PRIORITY_SHIFT) &
			                   Pm4::COMPUTE_PGM_RSRC1_PRIORITY_MASK;
			cs_regs.float_mode    = (value >> Pm4::COMPUTE_PGM_RSRC1_FLOAT_MODE_SHIFT) &
			                        Pm4::COMPUTE_PGM_RSRC1_FLOAT_MODE_MASK;
			cs_regs.dx10_clamp    = ((value >> Pm4::COMPUTE_PGM_RSRC1_DX10_CLAMP_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC1_DX10_CLAMP_MASK) != 0u;
			cs_regs.debug_mode    = ((value >> Pm4::COMPUTE_PGM_RSRC1_DEBUG_MODE_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC1_DEBUG_MODE_MASK) != 0u;
			cs_regs.ieee_mode     = ((value >> Pm4::COMPUTE_PGM_RSRC1_IEEE_MODE_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC1_IEEE_MODE_MASK) != 0u;
			cs_regs.fp16_overflow = ((value >> Pm4::COMPUTE_PGM_RSRC1_FP16_OVFL_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC1_FP16_OVFL_MASK) != 0u;
			cs_regs.threadgroup_configuration = ((value >> Pm4::COMPUTE_PGM_RSRC1_WGP_MODE_SHIFT) &
			                                     Pm4::COMPUTE_PGM_RSRC1_WGP_MODE_MASK) != 0u;
			cs_regs.require_forward_progress =
			    ((value >> Pm4::COMPUTE_PGM_RSRC1_FWD_PROGRESS_SHIFT) &
			     Pm4::COMPUTE_PGM_RSRC1_FWD_PROGRESS_MASK) != 0u;
			break;
		case Pm4::COMPUTE_PGM_RSRC2:
			cs_regs.scratch_en     = ((value >> Pm4::COMPUTE_PGM_RSRC2_SCRATCH_EN_SHIFT) &
			                          Pm4::COMPUTE_PGM_RSRC2_SCRATCH_EN_MASK) != 0u;
			cs_regs.user_sgpr      = (value >> Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_MASK;
			cs_regs.tgid_x_en      = ((value >> Pm4::COMPUTE_PGM_RSRC2_TGID_X_EN_SHIFT) &
			                          Pm4::COMPUTE_PGM_RSRC2_TGID_X_EN_MASK) != 0u;
			cs_regs.tgid_y_en      = ((value >> Pm4::COMPUTE_PGM_RSRC2_TGID_Y_EN_SHIFT) &
			                          Pm4::COMPUTE_PGM_RSRC2_TGID_Y_EN_MASK) != 0u;
			cs_regs.tgid_z_en      = ((value >> Pm4::COMPUTE_PGM_RSRC2_TGID_Z_EN_SHIFT) &
			                          Pm4::COMPUTE_PGM_RSRC2_TGID_Z_EN_MASK) != 0u;
			cs_regs.tg_size_en     = ((value >> Pm4::COMPUTE_PGM_RSRC2_TG_SIZE_EN_SHIFT) &
			                          Pm4::COMPUTE_PGM_RSRC2_TG_SIZE_EN_MASK) != 0u;
			cs_regs.tidig_comp_cnt = (value >> Pm4::COMPUTE_PGM_RSRC2_TIDIG_COMP_CNT_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_TIDIG_COMP_CNT_MASK;
			cs_regs.lds_size       = (value >> Pm4::COMPUTE_PGM_RSRC2_LDS_SIZE_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_LDS_SIZE_MASK;
			break;
		case Pm4::COMPUTE_PGM_RSRC3:
			cs_regs.shared_vgprs =
			    (value >> Pm4::COMPUTE_PGM_RSRC3_SHARED_VGPRS_SHIFT) &
			    Pm4::COMPUTE_PGM_RSRC3_SHARED_VGPRS_MASK;
			break;
		case Pm4::COMPUTE_NUM_THREAD_X: cs_regs.num_thread_x = value; break;
		case Pm4::COMPUTE_NUM_THREAD_Y: cs_regs.num_thread_y = value; break;
		case Pm4::COMPUTE_NUM_THREAD_Z: cs_regs.num_thread_z = value; break;
		case Pm4::COMPUTE_START_X:
		case Pm4::COMPUTE_START_Y:
		case Pm4::COMPUTE_START_Z:
		case Pm4::COMPUTE_RESOURCE_LIMITS:
		case Pm4::COMPUTE_TMPRING_SIZE:
		case Pm4::COMPUTE_PACE_ID: break;
		default: return false;
	}
	return true;
}

}
