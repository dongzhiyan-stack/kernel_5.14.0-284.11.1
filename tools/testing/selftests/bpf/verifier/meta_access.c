{
	"meta access, test1",
	.insns = {
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1,
		    offsetof(struct xdp_md, data_meta)),
	BPF_LDX_MEM(BPF_W, BPF_REG_3, BPF_REG_1, offsetof(struct xdp_md, data)),
	BPF_MOV64_REG(BPF_REG_0, BPF_REG_2),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_0, 8),
	BPF_JMP_REG(BPF_JGT, BPF_REG_0, BPF_REG_3, 1),
	BPF_LDX_MEM(BPF_B, BPF_REG_0, BPF_REG_2, 0),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.result = ACCEPT,
	.prog_type = BPF_PROG_TYPE_XDP,
},
{
	"meta access, test2",
	.insns = {
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1,
		    offsetof(struct xdp_md, data_meta)),
	BPF_LDX_MEM(BPF_W, BPF_REG_3, BPF_REG_1, offsetof(struct xdp_md, data)),
	BPF_MOV64_REG(BPF_REG_0, BPF_REG_2),
	BPF_ALU64_IMM(BPF_SUB, BPF_REG_0, 8),
	BPF_MOV64_REG(BPF_REG_4, BPF_REG_2),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_4, 8),
	BPF_JMP_REG(BPF_JGT, BPF_REG_4, BPF_REG_3, 1),
	BPF_LDX_MEM(BPF_B, BPF_REG_0, BPF_REG_0, 0),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.result = REJECT,
	.errstr = "invalid access to packet, off=-8",
	.prog_type = BPF_PROG_TYPE_XDP,
},
{
	"meta access, test3",
	.insns = {
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1,
		    offsetof(struct xdp_md, data_meta)),
	BPF_LDX_MEM(BPF_W, BPF_REG_3, BPF_REG_1,
		    offsetof(struct xdp_md, data_end)),
	BPF_MOV64_REG(BPF_REG_0, BPF_REG_2),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_0, 8),
	BPF_JMP_REG(BPF_JGT, BPF_REG_0, BPF_REG_3, 1),
	BPF_LDX_MEM(BPF_B, BPF_REG_0, BPF_REG_2, 0),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.result = REJECT,
	.errstr = "invalid access to packet",
	.prog_type = BPF_PROG_TYPE_XDP,
},
{
	"meta access, test4",
	.insns = {
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1,
		    offsetof(struct xdp_md, data_meta)),
	BPF_LDX_MEM(BPF_W, BPF_REG_3, BPF_REG_1,
		    offsetof(struct xdp_md, data_end)),
	BPF_LDX_MEM(BPF_W, BPF_REG_4, BPF_REG_1, offsetof(struct xdp_md, data)),
	BPF_MOV64_REG(BPF_REG_0, BPF_REG_4),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_0, 8),
	BPF_JMP_REG(BPF_JGT, BPF_REG_0, BPF_REG_3, 1),
	BPF_LDX_MEM(BPF_B, BPF_REG_0, BPF_REG_2, 0),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.result = REJECT,
	.errstr = "invalid access to packet",
	.prog_type = BPF_PROG_TYPE_XDP,
},
{
	"meta access, test5",
	.insns = {
	BPF_LDX_MEM(BPF_W, BPF_REG_3, BPF_REG_1,
		    offsetof(struct xdp_md, data_meta)),
	BPF_LDX_MEM(BPF_W, BPF_REG_4, BPF_REG_1, offsetof(struct xdp_md, data)),
	BPF_MOV64_REG(BPF_REG_0, BPF_REG_3),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_0, 8),
	BPF_JMP_REG(BPF_JGT, BPF_REG_0, BPF_REG_4, 3),
	BPF_MOV64_IMM(BPF_REG_2, -8),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_xdp_adjust_meta),
	BPF_LDX_MEM(BPF_B, BPF_REG_0, BPF_REG_3, 0),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.result = REJECT,
	.errstr = "R3 !read_ok",
	.prog_type = BPF_PROG_TYPE_XDP,
},
{
	"meta access, test6",
	.insns = {
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1,
		    offsetof(struct xdp_md, data_meta)),
	BPF_LDX_MEM(BPF_W, BPF_REG_3, BPF_REG_1, offsetof(struct xdp_md, data)),
	BPF_MOV64_REG(BPF_REG_0, BPF_REG_3),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_0, 8),
	BPF_MOV64_REG(BPF_REG_4, BPF_REG_2),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_4, 8),
	BPF_JMP_REG(BPF_JGT, BPF_REG_4, BPF_REG_0, 1),
	BPF_LDX_MEM(BPF_B, BPF_REG_0, BPF_REG_2, 0),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.result = REJECT,
	.errstr = "invalid access to packet",
	.prog_type = BPF_PROG_TYPE_XDP,
},
{
	"meta access, test7",
	.insns = {
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1,
		    offsetof(struct xdp_md, data_meta)),
	BPF_LDX_MEM(BPF_W, BPF_REG_3, BPF_REG_1, offsetof(struct xdp_md, data)),
	BPF_MOV64_REG(BPF_REG_0, BPF_REG_3),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_0, 8),
	BPF_MOV64_REG(BPF_REG_4, BPF_REG_2),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_4, 8),
	BPF_JMP_REG(BPF_JGT, BPF_REG_4, BPF_REG_3, 1),
	BPF_LDX_MEM(BPF_B, BPF_REG_0, BPF_REG_2, 0),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.result = ACCEPT,
	.prog_type = BPF_PROG_TYPE_XDP,
},
{
	"meta access, test8",
	.insns = {
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1,
		    offsetof(struct xdp_md, data_meta)),
	BPF_LDX_MEM(BPF_W, BPF_REG_3, BPF_REG_1, offsetof(struct xdp_md, data)),
	BPF_MOV64_REG(BPF_REG_4, BPF_REG_2),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_4, 0xFFFF),
	BPF_JMP_REG(BPF_JGT, BPF_REG_4, BPF_REG_3, 1),
	BPF_LDX_MEM(BPF_B, BPF_REG_0, BPF_REG_2, 0),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.result = ACCEPT,
	.prog_type = BPF_PROG_TYPE_XDP,
},
{
	"meta access, test9",
	.insns = {
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1,
		    offsetof(struct xdp_md, data_meta)),
	BPF_LDX_MEM(BPF_W, BPF_REG_3, BPF_REG_1, offsetof(struct xdp_md, data)),
	BPF_MOV64_REG(BPF_REG_4, BPF_REG_2),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_4, 0xFFFF),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_4, 1),
	BPF_JMP_REG(BPF_JGT, BPF_REG_4, BPF_REG_3, 1),
	BPF_LDX_MEM(BPF_B, BPF_REG_0, BPF_REG_2, 0),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.result = REJECT,
	.errstr = "invalid access to packet",
	.prog_type = BPF_PROG_TYPE_XDP,
},
{
	"meta access, test10",
	.insns = {
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1,
		    offsetof(struct xdp_md, data_meta)),
	BPF_LDX_MEM(BPF_W, BPF_REG_3, BPF_REG_1, offsetof(struct xdp_md, data)),
	BPF_LDX_MEM(BPF_W, BPF_REG_4, BPF_REG_1,
		    offsetof(struct xdp_md, data_end)),
	BPF_MOV64_IMM(BPF_REG_5, 42),
	BPF_MOV64_IMM(BPF_REG_6, 24),
	BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_5, -8),
	BPF_ATOMIC_OP(BPF_DW, BPF_ADD, BPF_REG_10, BPF_REG_6, -8),
	BPF_LDX_MEM(BPF_DW, BPF_REG_5, BPF_REG_10, -8),
	BPF_JMP_IMM(BPF_JGT, BPF_REG_5, 100, 6),
	BPF_ALU64_REG(BPF_ADD, BPF_REG_3, BPF_REG_5),
	BPF_MOV64_REG(BPF_REG_5, BPF_REG_3),
	BPF_MOV64_REG(BPF_REG_6, BPF_REG_2),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_6, 8),
	BPF_JMP_REG(BPF_JGT, BPF_REG_6, BPF_REG_5, 1),
	BPF_LDX_MEM(BPF_B, BPF_REG_2, BPF_REG_2, 0),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.result = REJECT,
	.errstr = "invalid access to packet",
	.prog_type = BPF_PROG_TYPE_XDP,
},
{
	"meta access, test11",
	.insns = {
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1,
		    offsetof(struct xdp_md, data_meta)),
	BPF_LDX_MEM(BPF_W, BPF_REG_3, BPF_REG_1, offsetof(struct xdp_md, data)),
	BPF_MOV64_IMM(BPF_REG_5, 42),
	BPF_MOV64_IMM(BPF_REG_6, 24),
	BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_5, -8),
	BPF_ATOMIC_OP(BPF_DW, BPF_ADD, BPF_REG_10, BPF_REG_6, -8),
	BPF_LDX_MEM(BPF_DW, BPF_REG_5, BPF_REG_10, -8),
	BPF_JMP_IMM(BPF_JGT, BPF_REG_5, 100, 6),
	BPF_ALU64_REG(BPF_ADD, BPF_REG_2, BPF_REG_5),
	BPF_MOV64_REG(BPF_REG_5, BPF_REG_2),
	BPF_MOV64_REG(BPF_REG_6, BPF_REG_2),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_6, 8),
	BPF_JMP_REG(BPF_JGT, BPF_REG_6, BPF_REG_3, 1),
	BPF_LDX_MEM(BPF_B, BPF_REG_5, BPF_REG_5, 0),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.result = ACCEPT,
	.prog_type = BPF_PROG_TYPE_XDP,
},
{
	"meta access, test12",
	.insns = {
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1,
		    offsetof(struct xdp_md, data_meta)),
	BPF_LDX_MEM(BPF_W, BPF_REG_3, BPF_REG_1, offsetof(struct xdp_md, data)),
	BPF_LDX_MEM(BPF_W, BPF_REG_4, BPF_REG_1,
		    offsetof(struct xdp_md, data_end)),
	BPF_MOV64_REG(BPF_REG_5, BPF_REG_3),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_5, 16),
	BPF_JMP_REG(BPF_JGT, BPF_REG_5, BPF_REG_4, 5),
	BPF_LDX_MEM(BPF_B, BPF_REG_0, BPF_REG_3, 0),
	BPF_MOV64_REG(BPF_REG_5, BPF_REG_2),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_5, 16),
	BPF_JMP_REG(BPF_JGT, BPF_REG_5, BPF_REG_3, 1),
	BPF_LDX_MEM(BPF_B, BPF_REG_0, BPF_REG_2, 0),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.result = ACCEPT,
	.prog_type = BPF_PROG_TYPE_XDP,
},
