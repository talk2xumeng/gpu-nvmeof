/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2016 6WIND S.A.
 * Copyright 2016 Mellanox Technologies, Ltd
 *
 * devx_ifc.h  --  DevX 命令的 PRM 结构(位宽数组,只用来算偏移)
 *
 * 从 DPDK v24.07 drivers/common/mlx5/mlx5_prm.h 原样摘出,只保留
 * CREATE_CQ / CREATE_QP / RST2INIT / INIT2RTR / RTR2RTS 用到的结构。
 * rdma-core 自带的 mlx5_ifc.h 没有 cqc / create_cq_in;内核那份缺
 * cq_umem_id / wq_umem_id / dbr_umem_id(并进了 reserved)。
 * 用 mlx5dv.h 的 DEVX_SET / DEVX_GET / DEVX_ADDR_OF 读写。
 */

#ifndef DEVX_IFC_H
#define DEVX_IFC_H

#include <stdint.h>

typedef uint8_t u8;

#define MLX5_CMD_OP_CREATE_CQ		0x400
#define MLX5_CMD_OP_DESTROY_CQ		0x401
#define MLX5_CMD_OP_CREATE_QP		0x500
#define MLX5_CMD_OP_DESTROY_QP		0x501
#define MLX5_CMD_OP_RST2INIT_QP		0x502
#define MLX5_CMD_OP_INIT2RTR_QP		0x503
#define MLX5_CMD_OP_RTR2RTS_QP		0x504

#define MLX5_QPC_ST_RC			0x0
#define MLX5_QPC_PM_STATE_MIGRATED	0x3
#define MLX5_QPC_RQ_TYPE_ZERO_LEN	0x3

struct mlx5_ifc_cqc_bits {
	u8 status[0x4];
	u8 as_notify[0x1];
	u8 initiator_src_dct[0x1];
	u8 dbr_umem_valid[0x1];
	u8 ext_element[0x1];
	u8 cqe_sz[0x3];
	u8 cc[0x1];
	u8 reserved_at_c[0x1];
	u8 scqe_break_moderation_en[0x1];
	u8 oi[0x1];
	u8 cq_period_mode[0x2];
	u8 cqe_comp_en[0x1];
	u8 mini_cqe_res_format[0x2];
	u8 st[0x4];
	u8 always_armed_cq[0x1];
	u8 ext_element_type[0x3];
	u8 reserved_at_1c[0x2];
	u8 cqe_comp_layout[0x2];
	u8 dbr_umem_id[0x20];
	u8 reserved_at_40[0x14];
	u8 page_offset[0x6];
	u8 reserved_at_5a[0x2];
	u8 mini_cqe_res_format_ext[0x2];
	u8 cq_timestamp_format[0x2];
	u8 reserved_at_60[0x3];
	u8 log_cq_size[0x5];
	u8 uar_page[0x18];
	u8 reserved_at_80[0x4];
	u8 cq_period[0xc];
	u8 cq_max_count[0x10];
	u8 reserved_at_a0[0x18];
	u8 c_eqn[0x8];
	u8 reserved_at_c0[0x3];
	u8 log_page_size[0x5];
	u8 reserved_at_c8[0x18];
	u8 reserved_at_e0[0x20];
	u8 reserved_at_100[0x8];
	u8 last_notified_index[0x18];
	u8 reserved_at_120[0x8];
	u8 last_solicit_index[0x18];
	u8 reserved_at_140[0x8];
	u8 consumer_counter[0x18];
	u8 reserved_at_160[0x8];
	u8 producer_counter[0x18];
	u8 local_partition_id[0xc];
	u8 process_id[0x14];
	u8 reserved_at_1A0[0x20];
	u8 dbr_addr[0x40];
};

struct mlx5_ifc_create_cq_in_bits {
	u8 opcode[0x10];
	u8 uid[0x10];
	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];
	u8 reserved_at_40[0x40];
	struct mlx5_ifc_cqc_bits cq_context;
	u8 cq_umem_offset[0x40];
	u8 cq_umem_id[0x20];
	u8 cq_umem_valid[0x1];
	u8 reserved_at_2e1[0x1f];
	u8 reserved_at_300[0x580];
	u8 pas[];
};

struct mlx5_ifc_create_cq_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];
	u8 syndrome[0x20];
	u8 reserved_at_40[0x8];
	u8 cqn[0x18];
	u8 reserved_at_60[0x20];
};

struct mlx5_ifc_ads_bits {
	u8 fl[0x1];
	u8 free_ar[0x1];
	u8 reserved_at_2[0xe];
	u8 pkey_index[0x10];
	u8 reserved_at_20[0x8];
	u8 grh[0x1];
	u8 mlid[0x7];
	u8 rlid[0x10];
	u8 ack_timeout[0x5];
	u8 reserved_at_45[0x3];
	u8 src_addr_index[0x8];
	u8 reserved_at_50[0x4];
	u8 stat_rate[0x4];
	u8 hop_limit[0x8];
	u8 reserved_at_60[0x4];
	u8 tclass[0x8];
	u8 flow_label[0x14];
	u8 rgid_rip[16][0x8];
	u8 reserved_at_100[0x4];
	u8 f_dscp[0x1];
	u8 f_ecn[0x1];
	u8 reserved_at_106[0x1];
	u8 f_eth_prio[0x1];
	u8 ecn[0x2];
	u8 dscp[0x6];
	u8 udp_sport[0x10];
	u8 dei_cfi[0x1];
	u8 eth_prio[0x3];
	u8 sl[0x4];
	u8 vhca_port_num[0x8];
	u8 rmac_47_32[0x10];
	u8 rmac_31_0[0x20];
};

struct mlx5_ifc_qpc_bits {
	u8 state[0x4];
	u8 lag_tx_port_affinity[0x4];
	u8 st[0x8];
	u8 reserved_at_10[0x3];
	u8 pm_state[0x2];
	u8 reserved_at_15[0x1];
	u8 req_e2e_credit_mode[0x2];
	u8 offload_type[0x4];
	u8 end_padding_mode[0x2];
	u8 reserved_at_1e[0x2];
	u8 wq_signature[0x1];
	u8 block_lb_mc[0x1];
	u8 atomic_like_write_en[0x1];
	u8 latency_sensitive[0x1];
	u8 reserved_at_24[0x1];
	u8 drain_sigerr[0x1];
	u8 reserved_at_26[0x2];
	u8 pd[0x18];
	u8 mtu[0x3];
	u8 log_msg_max[0x5];
	u8 reserved_at_48[0x1];
	u8 log_rq_size[0x4];
	u8 log_rq_stride[0x3];
	u8 no_sq[0x1];
	u8 log_sq_size[0x4];
	u8 reserved_at_55[0x3];
	u8 ts_format[0x2];
	u8 reserved_at_5a[0x1];
	u8 rlky[0x1];
	u8 ulp_stateless_offload_mode[0x4];
	u8 counter_set_id[0x8];
	u8 uar_page[0x18];
	u8 reserved_at_80[0x8];
	u8 user_index[0x18];
	u8 reserved_at_a0[0x3];
	u8 log_page_size[0x5];
	u8 remote_qpn[0x18];
	struct mlx5_ifc_ads_bits primary_address_path;
	struct mlx5_ifc_ads_bits secondary_address_path;
	u8 log_ack_req_freq[0x4];
	u8 reserved_at_384[0x4];
	u8 log_sra_max[0x3];
	u8 reserved_at_38b[0x2];
	u8 retry_count[0x3];
	u8 rnr_retry[0x3];
	u8 reserved_at_393[0x1];
	u8 fre[0x1];
	u8 cur_rnr_retry[0x3];
	u8 cur_retry_count[0x3];
	u8 reserved_at_39b[0x5];
	u8 reserved_at_3a0[0x20];
	u8 reserved_at_3c0[0x8];
	u8 next_send_psn[0x18];
	u8 reserved_at_3e0[0x8];
	u8 cqn_snd[0x18];
	u8 reserved_at_400[0x8];
	u8 deth_sqpn[0x18];
	u8 reserved_at_420[0x20];
	u8 reserved_at_440[0x8];
	u8 last_acked_psn[0x18];
	u8 reserved_at_460[0x8];
	u8 ssn[0x18];
	u8 reserved_at_480[0x8];
	u8 log_rra_max[0x3];
	u8 reserved_at_48b[0x1];
	u8 atomic_mode[0x4];
	u8 rre[0x1];
	u8 rwe[0x1];
	u8 rae[0x1];
	u8 reserved_at_493[0x1];
	u8 page_offset[0x6];
	u8 reserved_at_49a[0x3];
	u8 cd_slave_receive[0x1];
	u8 cd_slave_send[0x1];
	u8 cd_master[0x1];
	u8 reserved_at_4a0[0x3];
	u8 min_rnr_nak[0x5];
	u8 next_rcv_psn[0x18];
	u8 reserved_at_4c0[0x8];
	u8 xrcd[0x18];
	u8 reserved_at_4e0[0x8];
	u8 cqn_rcv[0x18];
	u8 dbr_addr[0x40];
	u8 q_key[0x20];
	u8 reserved_at_560[0x5];
	u8 rq_type[0x3];
	u8 srqn_rmpn_xrqn[0x18];
	u8 reserved_at_580[0x8];
	u8 rmsn[0x18];
	u8 hw_sq_wqebb_counter[0x10];
	u8 sw_sq_wqebb_counter[0x10];
	u8 hw_rq_counter[0x20];
	u8 sw_rq_counter[0x20];
	u8 reserved_at_600[0x20];
	u8 reserved_at_620[0xf];
	u8 cgs[0x1];
	u8 cs_req[0x8];
	u8 cs_res[0x8];
	u8 dc_access_key[0x40];
	u8 reserved_at_680[0x3];
	u8 dbr_umem_valid[0x1];
	u8 reserved_at_684[0x9c];
	u8 dbr_umem_id[0x20];
};

struct mlx5_ifc_qpc_pas_list_bits {
	u8 pas[0][0x40];
};

struct mlx5_ifc_qpc_extension_bits {
	u8 reserved_at_0[0x2];
	u8 mmo[0x1];
	u8 reserved_at_3[0x5fd];
};

struct mlx5_ifc_qpc_extension_and_pas_list_bits {
	struct mlx5_ifc_qpc_extension_bits qpc_data_extension;
	u8 pas[][0x40];
};

struct mlx5_ifc_create_qp_in_bits {
	u8 opcode[0x10];
	u8 uid[0x10];
	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];
	u8 qpc_ext[0x1];
	u8 reserved_at_41[0x3f];
	u8 opt_param_mask[0x20];
	u8 reserved_at_a0[0x20];
	struct mlx5_ifc_qpc_bits qpc;
	u8 wq_umem_offset[0x40];
	u8 wq_umem_id[0x20];
	u8 wq_umem_valid[0x1];
	u8 reserved_at_861[0x1f];
	union {
		struct mlx5_ifc_qpc_pas_list_bits qpc_pas_list;
		struct mlx5_ifc_qpc_extension_and_pas_list_bits
					qpc_extension_and_pas_list;
	};
};

struct mlx5_ifc_create_qp_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];
	u8 syndrome[0x20];
	u8 reserved_at_40[0x8];
	u8 qpn[0x18];
	u8 reserved_at_60[0x20];
};

struct mlx5_ifc_rst2init_qp_in_bits {
	u8 opcode[0x10];
	u8 uid[0x10];
	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];
	u8 reserved_at_40[0x8];
	u8 qpn[0x18];
	u8 reserved_at_60[0x20];
	u8 opt_param_mask[0x20];
	u8 reserved_at_a0[0x20];
	struct mlx5_ifc_qpc_bits qpc;
	u8 reserved_at_800[0x80];
};

struct mlx5_ifc_rst2init_qp_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];
	u8 syndrome[0x20];
	u8 reserved_at_40[0x40];
};

struct mlx5_ifc_init2rtr_qp_in_bits {
	u8 opcode[0x10];
	u8 uid[0x10];
	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];
	u8 reserved_at_40[0x8];
	u8 qpn[0x18];
	u8 reserved_at_60[0x20];
	u8 opt_param_mask[0x20];
	u8 reserved_at_a0[0x20];
	struct mlx5_ifc_qpc_bits qpc;
	u8 reserved_at_800[0x80];
};

struct mlx5_ifc_init2rtr_qp_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];
	u8 syndrome[0x20];
	u8 reserved_at_40[0x40];
};

struct mlx5_ifc_rtr2rts_qp_in_bits {
	u8 opcode[0x10];
	u8 uid[0x10];
	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];
	u8 reserved_at_40[0x8];
	u8 qpn[0x18];
	u8 reserved_at_60[0x20];
	u8 opt_param_mask[0x20];
	u8 reserved_at_a0[0x20];
	struct mlx5_ifc_qpc_bits qpc;
	u8 reserved_at_800[0x80];
};

struct mlx5_ifc_rtr2rts_qp_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];
	u8 syndrome[0x20];
	u8 reserved_at_40[0x40];
};

#endif /* DEVX_IFC_H */
