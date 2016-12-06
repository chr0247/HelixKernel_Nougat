/* Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) "#%d " fmt "\n", __LINE__

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/dma-mapping.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/msm-sps.h>
#include <linux/msm-bus.h>
#include <linux/msm-bus-board.h>
#include <linux/i2c/i2c-msm-v2.h>

#ifdef DEBUG
static const enum msm_i2_debug_level DEFAULT_DBG_LVL = MSM_DBG;
#else
static const enum msm_i2_debug_level DEFAULT_DBG_LVL = MSM_ERR;
#endif

static bool i2c_msm_xfer_next_buf(struct i2c_msm_ctrl *ctrl);
static int i2c_msm_xfer_wait_for_completion(struct i2c_msm_ctrl *ctrl,
						struct completion *complete);
static int  i2c_msm_pm_resume(struct device *dev);
static void i2c_msm_pm_suspend(struct device *dev);
static void i2c_msm_clk_path_init(struct i2c_msm_ctrl *ctrl);

const char * const i2c_msm_mode_str_tbl[] = {
	"FIFO", "BLOCK", "DMA", "None",
};

static const u32 i2c_msm_fifo_block_sz_tbl[] = {16, 16 , 32, 0};

static const u32 i2c_msm_mode_to_reg_tbl[] = {
	0x0, 
	0x1, 
	0x3  
};

const char *i2c_msm_err_str_table[] = {
	[I2C_MSM_NO_ERR]     = "NONE",
	[I2C_MSM_ERR_NACK]   = "NACK: slave not responding, ensure its powered",
	[I2C_MSM_ERR_ARB_LOST] = "ARB_LOST",
	[I2C_MSM_ERR_BUS_ERR] = "BUS ERROR:noisy bus/unexpected start/stop tag",
	[I2C_MSM_ERR_TIMEOUT]  = "TIMEOUT_ERROR",
	[I2C_MSM_ERR_CORE_CLK] = "CLOCK OFF: Check Core Clock",
	[I2C_MSM_ERR_OVR_UNDR_RUN] = "OVER_UNDER_RUN_ERROR",
};

static void i2c_msm_dbg_dump_diag(struct i2c_msm_ctrl *ctrl,
				bool use_param_vals, u32 status, u32 qup_op)
{
	struct i2c_msm_xfer *xfer = &ctrl->xfer;
	const char *str = i2c_msm_err_str_table[xfer->err];
	char buf[I2C_MSM_REG_2_STR_BUF_SZ];

	if (!use_param_vals) {
		void __iomem        *base = ctrl->rsrcs.base;
		status = readl_relaxed(base + QUP_I2C_STATUS);
		qup_op = readl_relaxed(base + QUP_OPERATIONAL);
	}

	if (xfer->err == I2C_MSM_ERR_TIMEOUT) {
		if (!(status & QUP_BUS_MASTER))
			snprintf(buf, I2C_MSM_REG_2_STR_BUF_SZ,
				"%s(val:%dmsec) misconfigured GPIO or slave pulling bus line(s) low\n",
				str, jiffies_to_msecs(xfer->timeout));
		 else
			snprintf(buf, I2C_MSM_REG_2_STR_BUF_SZ,
			"%s(val:%dmsec)", str, jiffies_to_msecs(xfer->timeout));

		str = buf;
	}

	if ((xfer->err == I2C_MSM_ERR_NACK) && (xfer->msgs->addr == ctrl->rsrcs.nack_as_normal)) {
	} else {
		
		dev_err(ctrl->dev,
			"%s: msgs(n:%d cur:%d %s) bc(rx:%zu tx:%zu) mode:%s slv_addr:0x%0x MSTR_STS:0x%08x OPER:0x%08x\n",
			str, xfer->msg_cnt, xfer->cur_buf.msg_idx,
			xfer->cur_buf.is_rx ? "rx" : "tx", xfer->rx_cnt, xfer->tx_cnt,
			i2c_msm_mode_str_tbl[xfer->mode_id], xfer->msgs->addr,
			status, qup_op);
	}
}

static u32 i2c_msm_reg_io_modes_out_blk_sz(u32 qup_io_modes)
{
	return i2c_msm_fifo_block_sz_tbl[qup_io_modes & 0x3];
}

static u32 i2c_msm_reg_io_modes_in_blk_sz(u32 qup_io_modes)
{
	return i2c_msm_fifo_block_sz_tbl[BITS_AT(qup_io_modes, 5, 2)];
}

static const u32 i2c_msm_fifo_sz_table[] = {2, 4 , 8, 16};

static void i2c_msm_qup_fifo_calc_size(struct i2c_msm_ctrl *ctrl)
{
	u32 reg_data, output_fifo_size, input_fifo_size;
	struct i2c_msm_xfer_mode_fifo *fifo = &ctrl->xfer.fifo;

	
	if (fifo->input_fifo_sz && fifo->output_fifo_sz)
		return;

	reg_data = readl_relaxed(ctrl->rsrcs.base + QUP_IO_MODES);
	output_fifo_size  = BITS_AT(reg_data, 2, 2);
	input_fifo_size   = BITS_AT(reg_data, 7, 2);

	fifo->input_fifo_sz = i2c_msm_reg_io_modes_in_blk_sz(reg_data) *
					i2c_msm_fifo_sz_table[input_fifo_size];
	fifo->output_fifo_sz = i2c_msm_reg_io_modes_out_blk_sz(reg_data) *
					i2c_msm_fifo_sz_table[output_fifo_size];

	i2c_msm_dbg(ctrl, MSM_PROF, "QUP input-sz:%zu, input-sz:%zu",
			fifo->input_fifo_sz, fifo->output_fifo_sz);

}

static u8 *i2c_msm_tag_byte(struct i2c_msm_tag *tag, int byte_n)
{
	return ((u8 *)tag) + byte_n;
}

static u8 *i2c_msm_buf_to_ptr(struct i2c_msm_xfer_buf *buf)
{
	struct i2c_msm_xfer *xfer =
				container_of(buf, struct i2c_msm_xfer, cur_buf);
	struct i2c_msg *msg = xfer->msgs + buf->msg_idx;
	return msg->buf + buf->byte_idx;
}

static const struct i2c_msm_tag tag_lookup_table[2][2][2] = {
	{{{QUP_TAG2_DATA_WRITE                                   , 2},
	   {QUP_TAG2_DATA_READ                                   , 2} },
	
	  {{QUP_TAG2_DATA_WRITE_N_STOP                            , 2},
	   {QUP_TAG2_DATA_READ_N_STOP                             , 2} } } ,
	
	 {{{QUP_TAG2_START | (QUP_TAG2_DATA_WRITE           << 16), 4},
	   {QUP_TAG2_START | (QUP_TAG2_DATA_READ            << 16), 4} },
	
	  {{QUP_TAG2_START | (QUP_TAG2_DATA_WRITE_N_STOP    << 16), 4},
	   {QUP_TAG2_START | (QUP_TAG2_DATA_READ_N_STOP     << 16), 4} } },
};

static struct i2c_msm_tag i2c_msm_tag_create(bool is_new_addr, bool is_last_buf,
					bool is_rx, u8 buf_len, u8 slave_addr)
{
	struct i2c_msm_tag tag;
	
	is_new_addr = is_new_addr ? 1 : 0;
	is_last_buf = is_last_buf ? 1 : 0;
	is_rx = is_rx ? 1 : 0;

	tag = tag_lookup_table[is_new_addr][is_last_buf][is_rx];
	
	if (tag.len == I2C_MSM_TAG2_MAX_LEN) {
		*i2c_msm_tag_byte(&tag, 1) = slave_addr;
		*i2c_msm_tag_byte(&tag, 3) = buf_len;
	} else {
		*i2c_msm_tag_byte(&tag, 1) = buf_len;
	}

	return tag;
}

static int
i2c_msm_qup_state_wait_valid(struct i2c_msm_ctrl *ctrl,
			enum i2c_msm_qup_state state, bool only_valid)
{
	u32 status;
	void __iomem  *base     = ctrl->rsrcs.base;
	int ret      = 0;
	int read_cnt = 0;

	do {
		status = readl_relaxed(base + QUP_STATE);
		++read_cnt;

		if (status & QUP_STATE_VALID) {
			if (only_valid)
				goto poll_valid_end;
			else if ((state & QUP_I2C_MAST_GEN) &&
					(status & QUP_I2C_MAST_GEN))
				goto poll_valid_end;
			else if ((status & QUP_STATE_MASK) == state)
				goto poll_valid_end;
		}

		if (!(read_cnt % 100))
			usleep_range(1000, 1500);
	} while (read_cnt <= 1500);

	ret = -ETIMEDOUT;
	dev_err(ctrl->dev,
		"error timeout on polling for valid state. check core_clk\n");

poll_valid_end:
	if (!only_valid)
		i2c_msm_prof_evnt_add(ctrl, MSM_DBG, I2C_MSM_VALID_END,
				
				(((-ret) & 0xff) | ((state & 0xf) << 16)),
				read_cnt, status);

	return ret;
}

static int i2c_msm_qup_state_set(struct i2c_msm_ctrl *ctrl,
						enum i2c_msm_qup_state state)
{
	if (i2c_msm_qup_state_wait_valid(ctrl, 0, true))
		return -EIO;

	writel_relaxed(state, ctrl->rsrcs.base + QUP_STATE);

	if (i2c_msm_qup_state_wait_valid(ctrl, state, false))
		return -EIO;

	return 0;
}

static int i2c_msm_qup_sw_reset(struct i2c_msm_ctrl *ctrl)
{
	int ret;

	writel_relaxed(1, ctrl->rsrcs.base + QUP_SW_RESET);
	/*
	 * Ensure that QUP that reset state is written before waiting for a the
	 * reset state to be valid.
	 */
	wmb();
	ret = i2c_msm_qup_state_wait_valid(ctrl, QUP_STATE_RESET, false);
	if (ret) {
		if (atomic_read(&ctrl->xfer.is_active))
			ctrl->xfer.err = I2C_MSM_ERR_CORE_CLK;
		dev_err(ctrl->dev, "error on issuing QUP software-reset\n");
	}
	return ret;
}

static void
i2c_msm_qup_xfer_init_reset_state(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer *xfer = &ctrl->xfer;
	void __iomem * const base = ctrl->rsrcs.base;
	u32  mx_rd_cnt     = 0;
	u32  mx_wr_cnt     = 0;
	u32  mx_in_cnt     = 0;
	u32  mx_out_cnt    = 0;
	u32  no_input      = 0;
	u32  no_output     = 0;
	u32  input_mode    = i2c_msm_mode_to_reg_tbl[xfer->mode_id] << 12;
	u32  output_mode   = i2c_msm_mode_to_reg_tbl[xfer->mode_id] << 10;
	u32  config_reg;
	u32  io_modes_reg;
	u32  op_mask;
	u32  rx_cnt = 0;
	u32  tx_cnt = 0;
	if (xfer->mode_id != I2C_MSM_XFER_MODE_DMA) {
		rx_cnt   = xfer->rx_cnt + xfer->rx_ovrhd_cnt;
		tx_cnt   = xfer->tx_cnt + xfer->tx_ovrhd_cnt;
		no_input = rx_cnt  ? 0 : QUP_NO_INPUT;

		switch (xfer->mode_id) {
		case I2C_MSM_XFER_MODE_FIFO:
			mx_rd_cnt  = rx_cnt;
			mx_wr_cnt  = tx_cnt;
			break;
		case I2C_MSM_XFER_MODE_BLOCK:
			mx_in_cnt  = rx_cnt;
			mx_out_cnt = tx_cnt;
			break;
		default:
			break;
		}
	}

	
	writel_relaxed(mx_in_cnt,  base + QUP_MX_INPUT_COUNT);
	writel_relaxed(mx_out_cnt, base + QUP_MX_OUTPUT_COUNT);

	
	writel_relaxed(mx_rd_cnt, base + QUP_MX_READ_COUNT);
	writel_relaxed(mx_wr_cnt, base + QUP_MX_WRITE_COUNT);

	config_reg = readl_relaxed(base + QUP_CONFIG);
	config_reg &=
	      ~(QUP_NO_INPUT | QUP_NO_OUPUT | QUP_N_MASK | QUP_MINI_CORE_MASK);
	config_reg |= (no_input | no_output | QUP_N_VAL |
							QUP_MINI_CORE_I2C_VAL);
	writel_relaxed(config_reg, base + QUP_CONFIG);

	io_modes_reg = readl_relaxed(base + QUP_IO_MODES);
	io_modes_reg &=
	   ~(QUP_INPUT_MODE | QUP_OUTPUT_MODE | QUP_PACK_EN | QUP_UNPACK_EN
	     | QUP_OUTPUT_BIT_SHIFT_EN);
	io_modes_reg |=
	   (input_mode | output_mode | QUP_PACK_EN | QUP_UNPACK_EN);
	writel_relaxed(io_modes_reg, base + QUP_IO_MODES);

	op_mask = (xfer->mode_id == I2C_MSM_XFER_MODE_DMA) ?
		    (QUP_INPUT_SERVICE_MASK | QUP_OUTPUT_SERVICE_MASK) : 0 ;
	writel_relaxed(op_mask, base + QUP_OPERATIONAL_MASK);
	/* Ensure that QUP configuration is written before leaving this func */
	wmb();
}

struct i2c_msm_clk_div_fld {
	u32                clk_freq_out;
	u8                 fs_div;
	u8                 ht_div;
};

static struct i2c_msm_clk_div_fld i2c_msm_clk_div_map[] = {
	{KHz(100), 124, 62},
	{KHz(400),  28, 14},
	{KHz(1000),  8,  5},
};

static int i2c_msm_set_mstr_clk_ctl(struct i2c_msm_ctrl *ctrl, int fs_div,
			int ht_div, int noise_rjct_scl, int noise_rjct_sda)
{
	int ret = 0;
	int i;
	u32 reg_val = 0;
	struct i2c_msm_clk_div_fld *itr = i2c_msm_clk_div_map;

	
	reg_val = I2C_MSM_SCL_NOISE_REJECTION(reg_val, noise_rjct_scl);
	reg_val = I2C_MSM_SDA_NOISE_REJECTION(reg_val, noise_rjct_sda);

	for (i = 0; i < ARRAY_SIZE(i2c_msm_clk_div_map); ++i, ++itr) {
		if (ctrl->rsrcs.clk_freq_out == itr->clk_freq_out) {
			if (!fs_div)
				fs_div = itr->fs_div;
			if (!ht_div)
				ht_div = itr->ht_div;
			break;
		}
	}
	if (!fs_div) {
		dev_err(ctrl->dev, "For non-standard clock freq:%dKHz\n"
		"clk divider value fs_div should be supply by client through\n"
		"device tree\n", (ctrl->rsrcs.clk_freq_out / 1000));
		return -EINVAL;
	}

	
	ctrl->mstr_clk_ctl = (reg_val & (~0xff07ff)) | ((ht_div & 0xff) << 16)
							|(fs_div & 0xff);

	return ret;
}

static void i2c_msm_qup_xfer_init_run_state(struct i2c_msm_ctrl *ctrl)
{
	void __iomem *base = ctrl->rsrcs.base;
	writel_relaxed(ctrl->mstr_clk_ctl, base + QUP_I2C_MASTER_CLK_CTL);

	/* Ensure that QUP configuration is written before leaving this func */
	wmb();

	if (ctrl->dbgfs.dbg_lvl == MSM_DBG) {
		dev_info(ctrl->dev,
			"QUP state after programming for next transfers\n");
		i2c_msm_dbg_qup_reg_dump(ctrl);
	}
}

static void i2c_msm_fifo_wr_word(struct i2c_msm_ctrl *ctrl, u32 data)
{
	writel_relaxed(data, ctrl->rsrcs.base + QUP_OUT_FIFO_BASE);
	i2c_msm_dbg(ctrl, MSM_DBG, "OUT-FIFO:0x%08x", data);
}

static u32 i2c_msm_fifo_rd_word(struct i2c_msm_ctrl *ctrl, u32 *data)
{
	u32 val;

	val = readl_relaxed(ctrl->rsrcs.base + QUP_IN_FIFO_BASE);
	i2c_msm_dbg(ctrl, MSM_DBG, "IN-FIFO :0x%08x", val);

	if (data)
		*data = val;

	return val;
}

static void i2c_msm_fifo_wr_buf_flush(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_fifo *fifo = &ctrl->xfer.fifo;
	u32 *word;

	if (!fifo->out_buf_idx)
		return;

	word = (u32 *) fifo->out_buf;
	i2c_msm_fifo_wr_word(ctrl, *word);
	fifo->out_buf_idx = 0;
	*word = 0;
}

/*
 * i2c_msm_fifo_wr_buf:
 *
 * @len buf size (in bytes)
 * @return number of bytes from buf which have been processed (written to
 *         FIFO or kept in out buffer and will be written later)
 */
static size_t
i2c_msm_fifo_wr_buf(struct i2c_msm_ctrl *ctrl, u8 *buf, size_t len)
{
	struct i2c_msm_xfer_mode_fifo *fifo = &ctrl->xfer.fifo;
	int i;

	for (i = 0 ; i < len; ++i, ++buf) {

		fifo->out_buf[fifo->out_buf_idx] = *buf;
		++fifo->out_buf_idx;

		if (fifo->out_buf_idx == 4) {
			u32 *word = (u32 *) fifo->out_buf;

			i2c_msm_fifo_wr_word(ctrl, *word);
			fifo->out_buf_idx = 0;
			*word = 0;
		}
	}
	return i;
}

static size_t i2c_msm_fifo_xfer_wr_tag(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_buf *buf = &ctrl->xfer.cur_buf;
	size_t len = 0;

	if (ctrl->dbgfs.dbg_lvl >= MSM_DBG) {
		char str[I2C_MSM_REG_2_STR_BUF_SZ];
		dev_info(ctrl->dev, "tag.val:0x%llx tag.len:%d %s\n",
			buf->out_tag.val, buf->out_tag.len,
			i2c_msm_dbg_tag_to_str(&buf->out_tag, str,
								sizeof(str)));
	}

	if (buf->out_tag.len) {
		len = i2c_msm_fifo_wr_buf(ctrl, (u8 *) &buf->out_tag.val,
							buf->out_tag.len);

		if (len < buf->out_tag.len)
			goto done;

		buf->out_tag = (struct i2c_msm_tag) {0};
	}
done:
	return len;
}

static void i2c_msm_fifo_read_xfer_buf(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_buf *buf = &ctrl->xfer.cur_buf;
	struct i2c_msg          *msg = ctrl->xfer.msgs + buf->msg_idx;
	u8 *p_tag_val   = (u8 *) &buf->in_tag.val;
	int buf_need_bc = msg->len - buf->byte_idx;
	u8  word[4];
	int copy_bc;
	int word_idx;
	int word_bc;

	if (!buf->is_rx)
		return;

	while (buf_need_bc || buf->in_tag.len) {
		i2c_msm_fifo_rd_word(ctrl, (u32 *) word);
		word_bc  = sizeof(word);
		word_idx = 0;

		if (buf->in_tag.len) {
			copy_bc = min_t(int, word_bc, buf->in_tag.len);

			memcpy(p_tag_val + buf->in_tag.len, word, copy_bc);

			word_idx        += copy_bc;
			word_bc         -= copy_bc;
			buf->in_tag.len -= copy_bc;

			if ((ctrl->dbgfs.dbg_lvl >= MSM_DBG) &&
							!buf->in_tag.len) {
				char str[64];
				dev_info(ctrl->dev, "%s\n",
					i2c_msm_dbg_tag_to_str(&buf->in_tag,
							str, sizeof(str)));
			}
		}

		
		copy_bc = min_t(int, word_bc, buf_need_bc);
		memcpy(msg->buf + buf->byte_idx, word + word_idx, copy_bc);

		buf->byte_idx += copy_bc;
		buf_need_bc   -= copy_bc;
	}
}

static void i2c_msm_fifo_write_xfer_buf(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_buf *buf  = &ctrl->xfer.cur_buf;
	size_t len;
	size_t tag_len;

	tag_len = buf->out_tag.len;
	len = i2c_msm_fifo_xfer_wr_tag(ctrl);
	if (len < tag_len) {
		dev_err(ctrl->dev, "error on writing tag to out FIFO\n");
		return;
	}

	if (!buf->is_rx) {
		if (ctrl->dbgfs.dbg_lvl >= MSM_DBG) {
			char str[I2C_MSM_REG_2_STR_BUF_SZ];
			int  offset = 0;
			u8  *p      = i2c_msm_buf_to_ptr(buf);
			int  i;

			for (i = 0 ; i < len; ++i, ++p)
				offset += snprintf(str + offset,
						   sizeof(str) - offset,
						   "0x%x ", *p);
			dev_info(ctrl->dev, "data: %s\n", str);
		}

		len = i2c_msm_fifo_wr_buf(ctrl, i2c_msm_buf_to_ptr(buf),
						buf->len);
		if (len < buf->len)
			dev_err(ctrl->dev, "error on xfering buf with FIFO\n");
	}
}

static int i2c_msm_fifo_xfer_process(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_buf first_buf = ctrl->xfer.cur_buf;
	int ret;

	
	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_PAUSE);
	if (ret < 0)
		return ret;

	
	while (i2c_msm_xfer_next_buf(ctrl))
		i2c_msm_fifo_write_xfer_buf(ctrl);

	i2c_msm_fifo_wr_buf_flush(ctrl);

	ctrl->xfer.cur_buf = first_buf;

	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_RUN);
	if (ret < 0)
		return ret;

	
	ret = i2c_msm_xfer_wait_for_completion(ctrl, &ctrl->xfer.complete);
	if (ret < 0)
		return ret;

	
	while (i2c_msm_xfer_next_buf(ctrl))
		i2c_msm_fifo_read_xfer_buf(ctrl);

	return 0;
}

static int i2c_msm_fifo_xfer(struct i2c_msm_ctrl *ctrl)
{
	int ret;

	i2c_msm_dbg(ctrl, MSM_DBG, "Starting FIFO transfer");

	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_RESET);
	if (ret < 0)
		return ret;

	
	i2c_msm_qup_xfer_init_reset_state(ctrl);

	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_RUN);
	if (ret < 0)
		return ret;

	
	i2c_msm_qup_xfer_init_run_state(ctrl);

	ret = i2c_msm_fifo_xfer_process(ctrl);

	return ret;
}

static int i2c_msm_blk_init_struct(struct i2c_msm_ctrl *ctrl)
{
	u32 reg_data = readl_relaxed(ctrl->rsrcs.base + QUP_IO_MODES);
	int ret;
	struct i2c_msm_xfer_mode_blk *blk = &ctrl->xfer.blk;

	blk->in_blk_sz  = i2c_msm_reg_io_modes_in_blk_sz(reg_data),
	blk->out_blk_sz = i2c_msm_reg_io_modes_out_blk_sz(reg_data),

	blk->tx_cache = kmalloc(blk->out_blk_sz, GFP_KERNEL);
	if (!blk->tx_cache) {
		dev_err(ctrl->dev,
		"error on allocating memory for block tx_cache. malloc(size:%zu)\n",
		 blk->out_blk_sz);
		ret = -ENOMEM;
		goto out_buf_err;
	}

	blk->rx_cache = kmalloc(blk->in_blk_sz, GFP_KERNEL);
	if (!blk->tx_cache) {
		dev_err(ctrl->dev,
		"error on allocating memory for block tx_cache. malloc(size:%zu)\n",
		 blk->out_blk_sz);
		ret = -ENOMEM;
		goto in_buf_err;
	}

	blk->is_init = true;
	return 0;

in_buf_err:
	kfree(blk->tx_cache);
out_buf_err:

	return ret;
}

static int i2c_msm_blk_wr_flush(struct i2c_msm_ctrl *ctrl)
{
	int byte_num;
	int ret = 0;
	struct i2c_msm_xfer_mode_blk *blk = &ctrl->xfer.blk;
	u32 *buf_u32_ptr;

	if (!blk->tx_cache_idx)
		return 0;

	
	ret = i2c_msm_xfer_wait_for_completion(ctrl, &blk->wait_tx_blk);
	if (ret)
		return ret;

	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_PAUSE);
	if (ret < 0)
		return ret;
	i2c_msm_dbg(ctrl, MSM_DBG, "OUT-BLK:%*phC", blk->tx_cache_idx,
							blk->tx_cache);

	for (byte_num = 0; byte_num < blk->tx_cache_idx;
						byte_num += sizeof(u32)) {
		buf_u32_ptr = (u32 *) (blk->tx_cache + byte_num);
		writel_relaxed(*buf_u32_ptr,
					ctrl->rsrcs.base + QUP_OUT_FIFO_BASE);
		*buf_u32_ptr = 0;
	}

	
	blk->tx_cache_idx = 0;
	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_RUN);
	if (ret < 0)
		return ret;

	return ret;
}

/*
 * i2c_msm_blk_wr_buf:
 *
 * @len buf size (in bytes)
 * @return number of bytes from buf which have been processed (written to
 *         FIFO or kept in out buffer and will be written later)
 */
static int
i2c_msm_blk_wr_buf(struct i2c_msm_ctrl *ctrl, const u8 *buf, int len)
{
	struct i2c_msm_xfer_mode_blk *blk = &ctrl->xfer.blk;
	int byte_num;
	int ret = 0;

	for (byte_num = 0; byte_num < len; ++byte_num, ++buf) {
		blk->tx_cache[blk->tx_cache_idx] = *buf;
		++blk->tx_cache_idx;

		
		if (blk->tx_cache_idx == blk->out_blk_sz) {
			ret = i2c_msm_blk_wr_flush(ctrl);
			if (ret)
				return ret;
		}
	}
	return byte_num;
}

static int i2c_msm_blk_xfer_wr_tag(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_buf *buf = &ctrl->xfer.cur_buf;
	int len = 0;
	if (!buf->out_tag.len)
		return 0;

	len = i2c_msm_blk_wr_buf(ctrl, (u8 *) &buf->out_tag.val,
							buf->out_tag.len);
	if (len != buf->out_tag.len)
		return -EFAULT;

	buf->out_tag = (struct i2c_msm_tag) {0};
	return 0;
}

static int i2c_msm_blk_wr_xfer_buf(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_buf *buf  = &ctrl->xfer.cur_buf;
	int len;
	int ret;
	ret = i2c_msm_blk_xfer_wr_tag(ctrl);
	if (ret)
		return ret;

	len = i2c_msm_blk_wr_buf(ctrl, i2c_msm_buf_to_ptr(buf), buf->len);
	if (len < buf->len)
		return -EFAULT;

	buf->byte_idx += len;
	return 0;
}

static int i2c_msm_blk_rd_blk(struct i2c_msm_ctrl *ctrl, int need_bc)
{
	int byte_num;
	int ret = 0;
	struct i2c_msm_xfer_mode_blk *blk = &ctrl->xfer.blk;
	u32 *cache_ptr = (u32 *) blk->rx_cache;
	int read_bc    = min_t(int, blk->in_blk_sz, need_bc);

	
	ret = i2c_msm_xfer_wait_for_completion(ctrl, &blk->wait_rx_blk);
	if (ret)
		return ret;

	
	for (byte_num = 0; byte_num < blk->in_blk_sz;
					byte_num += sizeof(u32)) {
		if (byte_num < read_bc) {
			*cache_ptr = readl_relaxed(ctrl->rsrcs.base +
							QUP_IN_FIFO_BASE);
			++cache_ptr;
		}
	}
	blk->rx_cache_idx = 0;
	return read_bc;
}

static int i2c_msm_blk_rd_xfer_buf(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_blk *blk = &ctrl->xfer.blk;
	struct i2c_msm_xfer_buf *buf      = &ctrl->xfer.cur_buf;
	struct i2c_msg *msg               = ctrl->xfer.msgs + buf->msg_idx;
	int    copy_bc;         
	int    cache_avail_bc;
	int    ret = 0;

	
	ret = i2c_msm_blk_xfer_wr_tag(ctrl);
	if (ret)
		return ret;
	i2c_msm_blk_wr_flush(ctrl);

	while (buf->len || buf->in_tag.len) {
		cache_avail_bc = i2c_msm_blk_rd_blk(ctrl,
						buf->len + buf->in_tag.len);

		i2c_msm_dbg(ctrl, MSM_DBG, "IN-BLK:%*phC\n", cache_avail_bc,
					blk->rx_cache + blk->rx_cache_idx);

		if (cache_avail_bc < 0)
			return cache_avail_bc;

		
		if (buf->in_tag.len) {
			int discard_bc = min_t(int, cache_avail_bc,
							buf->in_tag.len);
			blk->rx_cache_idx += discard_bc;
			buf->in_tag.len   -= discard_bc;
			cache_avail_bc    -= discard_bc;
		}

		
		copy_bc = min_t(int, cache_avail_bc, buf->len);
		memcpy(msg->buf + buf->byte_idx,
			blk->rx_cache + blk->rx_cache_idx, copy_bc);

		blk->rx_cache_idx += copy_bc;
		buf->len          -= copy_bc;
		buf->byte_idx     += copy_bc;
	}
	return ret;
}

static int i2c_msm_blk_xfer(struct i2c_msm_ctrl *ctrl)
{
	int ret = 0;
	struct i2c_msm_xfer_buf      *buf = &ctrl->xfer.cur_buf;
	struct i2c_msm_xfer_mode_blk *blk = &ctrl->xfer.blk;

	if (!blk->is_init) {
		ret = i2c_msm_blk_init_struct(ctrl);
		if (!blk->is_init)
			return ret;
	}

	init_completion(&blk->wait_rx_blk);
	init_completion(&blk->wait_tx_blk);

	
	blk->complete_mask = QUP_MAX_OUTPUT_DONE_FLAG;
	if (ctrl->xfer.rx_cnt)
		blk->complete_mask |= QUP_MAX_INPUT_DONE_FLAG;

	
	blk->tx_cache_idx = 0;
	blk->rx_cache_idx = 0;

	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_RESET);
	if (ret < 0)
		return ret;

	
	i2c_msm_qup_xfer_init_reset_state(ctrl);

	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_RUN);
	if (ret < 0)
		return ret;

	
	i2c_msm_qup_xfer_init_run_state(ctrl);

	while (i2c_msm_xfer_next_buf(ctrl)) {
		if (buf->is_rx) {
			ret = i2c_msm_blk_rd_xfer_buf(ctrl);
			if (ret)
				return ret;
			if (buf->is_last) {
				ret = i2c_msm_xfer_wait_for_completion(ctrl,
							&blk->wait_rx_blk);
				if (!ret)
					complete(&ctrl->xfer.complete);
			}
		} else {
			ret = i2c_msm_blk_wr_xfer_buf(ctrl);
			if (ret)
				return ret;
		}
	}
	i2c_msm_blk_wr_flush(ctrl);
	return i2c_msm_xfer_wait_for_completion(ctrl, &ctrl->xfer.complete);
}

static int i2c_msm_dma_xfer_prepare(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_dma *dma  = &ctrl->xfer.dma;
	struct i2c_msm_xfer_buf      *buf  = &ctrl->xfer.cur_buf;
	struct i2c_msm_dma_chan      *tx = &dma->chan[I2C_MSM_DMA_TX];
	struct i2c_msm_dma_chan      *rx = &dma->chan[I2C_MSM_DMA_RX];
	struct i2c_msm_dma_buf *dma_buf;
	int                     rem_buf_cnt = I2C_MSM_DMA_DESC_ARR_SIZ;
	struct i2c_msg         *cur_msg;
	enum dma_data_direction buf_dma_dirctn;
	struct i2c_msm_dma_mem  data;
	u8        *tag_arr_itr_vrtl_addr;
	dma_addr_t tag_arr_itr_phy_addr;

	tx->desc_cnt_cur    = 0;
	rx->desc_cnt_cur    = 0;
	dma->buf_arr_cnt      = 0;
	dma_buf               = dma->buf_arr;
	tag_arr_itr_vrtl_addr = ((u8 *) dma->tag_arr.vrtl_addr);
	tag_arr_itr_phy_addr  = dma->tag_arr.phy_addr;

	for (; i2c_msm_xfer_next_buf(ctrl) && rem_buf_cnt;
		++dma_buf,
		tag_arr_itr_phy_addr  += sizeof(dma_addr_t),
		tag_arr_itr_vrtl_addr += sizeof(dma_addr_t)) {

		
		cur_msg        = ctrl->xfer.msgs + buf->msg_idx;
		data.vrtl_addr = cur_msg->buf + buf->byte_idx;
		if (buf->is_rx) {
			buf_dma_dirctn  = DMA_FROM_DEVICE;
			rx->desc_cnt_cur += 2; 
			tx->desc_cnt_cur += 1; 
		} else {
			buf_dma_dirctn  = DMA_TO_DEVICE;
			tx->desc_cnt_cur += 2; 
		}

		
		if (buf->is_last) {
			
			ctrl->xfer.rx_ovrhd_cnt += 2; 
			ctrl->xfer.tx_ovrhd_cnt += 2; 

			tx->desc_cnt_cur++;
			rx->desc_cnt_cur++;
		}

		if ((rx->desc_cnt_cur >= I2C_MSM_DMA_RX_SZ) ||
		    (tx->desc_cnt_cur >= I2C_MSM_DMA_TX_SZ))
			return -ENOMEM;

		data.phy_addr = dma_map_single(ctrl->dev, data.vrtl_addr,
						buf->len, buf_dma_dirctn);

		if (dma_mapping_error(ctrl->dev, data.phy_addr)) {
			dev_err(ctrl->dev,
			  "error DMA mapping DMA buffers, err:%lld buf_vrtl:0x%p data_len:%d dma_dir:%s\n",
			  (u64) data.phy_addr, data.vrtl_addr, buf->len,
			  ((buf_dma_dirctn == DMA_FROM_DEVICE)
				? "DMA_FROM_DEVICE" : "DMA_TO_DEVICE"));
			return -EFAULT;
		}

		
		*((u64 *)tag_arr_itr_vrtl_addr) =  buf->out_tag.val;

		i2c_msm_dbg(ctrl, MSM_DBG,
			"vrtl:0x%p phy:0x%llx val:0x%llx sizeof(dma_addr_t):%zu",
			tag_arr_itr_vrtl_addr, (u64) tag_arr_itr_phy_addr,
			*((u64 *)tag_arr_itr_vrtl_addr), sizeof(dma_addr_t));

		*dma_buf = (struct i2c_msm_dma_buf) {
			.ptr      = data,
			.len      = buf->len,
			.dma_dir  = buf_dma_dirctn,
			.is_rx    = buf->is_rx,
			.is_last  = buf->is_last,
			.tag      = (struct i2c_msm_dma_tag) {
				.buf = tag_arr_itr_phy_addr,
				.len = buf->out_tag.len,
			},
		};
		++dma->buf_arr_cnt;
		--rem_buf_cnt;
	}
	return 0;
}

static void i2c_msm_dma_xfer_unprepare(struct i2c_msm_ctrl *ctrl)
{
	int i;
	struct i2c_msm_dma_buf *buf_itr = ctrl->xfer.dma.buf_arr;

	for (i = 0 ; i < ctrl->xfer.dma.buf_arr_cnt ; ++i, ++buf_itr)
		dma_unmap_single(ctrl->dev, buf_itr->ptr.phy_addr, buf_itr->len,
							buf_itr->dma_dir);
}

static void i2c_msm_dma_callback_xfer_complete(void *dma_async_param)
{
	struct i2c_msm_ctrl *ctrl = dma_async_param;
	complete(&ctrl->xfer.complete);
}

static int i2c_msm_dma_xfer_process(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_dma *dma = &ctrl->xfer.dma;
	struct i2c_msm_dma_chan *tx       = &dma->chan[I2C_MSM_DMA_TX];
	struct i2c_msm_dma_chan *rx       = &dma->chan[I2C_MSM_DMA_RX];
	struct scatterlist *sg_rx         = NULL;
	struct scatterlist *sg_rx_itr     = NULL;
	struct scatterlist *sg_tx         = NULL;
	struct scatterlist *sg_tx_itr     = NULL;
	struct dma_async_tx_descriptor     *dma_desc_rx;
	struct dma_async_tx_descriptor     *dma_desc_tx;
	struct i2c_msm_dma_buf             *buf_itr;
	int  i;
	int  ret = 0;

	i2c_msm_dbg(ctrl, MSM_DBG, "Going to enqueue %zu buffers in DMA",
							dma->buf_arr_cnt);

	
	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_PAUSE);
	if (ret) {
		dev_err(ctrl->dev, "transition to pause state failed before DMA transaction :%d\n",
									ret);
		return ret;
	}

	sg_tx = kzalloc(sizeof(struct scatterlist) * tx->desc_cnt_cur,
								GFP_KERNEL);
	if (!sg_tx) {
		ret = -ENOMEM;
		goto dma_xfer_end;
	}
	sg_init_table(sg_tx, tx->desc_cnt_cur);
	sg_tx_itr = sg_tx;

	sg_rx = kzalloc(sizeof(struct scatterlist) * rx->desc_cnt_cur,
								GFP_KERNEL);
	if (!sg_rx) {
		ret = -ENOMEM;
		goto dma_xfer_end;
	}
	sg_init_table(sg_rx, rx->desc_cnt_cur);
	sg_rx_itr = sg_rx;

	buf_itr = dma->buf_arr;

	for (i = 0; i < dma->buf_arr_cnt ; ++i, ++buf_itr) {
		
		sg_dma_address(sg_tx_itr) = buf_itr->tag.buf;
		sg_dma_len(sg_tx_itr) = buf_itr->tag.len;
		++sg_tx_itr;

		if (buf_itr->is_rx) {
			
			sg_dma_address(sg_rx_itr) =
					ctrl->xfer.dma.input_tag.phy_addr;
			sg_dma_len(sg_rx_itr)     = QUP_BUF_OVERHD_BC;
			++sg_rx_itr;

			
			sg_dma_address(sg_rx_itr) = buf_itr->ptr.phy_addr;
			sg_dma_len(sg_rx_itr)     = buf_itr->len;
			++sg_rx_itr;
		} else {
			sg_dma_address(sg_tx_itr) = buf_itr->ptr.phy_addr;
			sg_dma_len(sg_tx_itr)     = buf_itr->len;
			++sg_tx_itr;
		}
	}

	
	sg_dma_address(sg_tx_itr) = dma->eot_n_flush_stop_tags.phy_addr;
	sg_dma_len(sg_tx_itr)     = QUP_BUF_OVERHD_BC;
	++sg_tx_itr;

	sg_dma_address(sg_rx_itr) = ctrl->xfer.dma.input_tag.phy_addr;
	sg_dma_len(sg_rx_itr)     = QUP_BUF_OVERHD_BC;
	++sg_rx_itr;

	dma_desc_tx = dmaengine_prep_slave_sg(tx->dma_chan,
						sg_tx,
						sg_tx_itr - sg_tx,
						tx->dir,
						(SPS_IOVEC_FLAG_EOT |
							SPS_IOVEC_FLAG_NWD));
	if (dma_desc_tx < 0) {
		dev_err(ctrl->dev, "error dmaengine_prep_slave_sg tx:%ld\n",
							PTR_ERR(dma_desc_tx));
		ret = PTR_ERR(dma_desc_tx);
		goto dma_xfer_end;
	}

	
	dma_desc_tx->callback       = i2c_msm_dma_callback_xfer_complete;
	dma_desc_tx->callback_param = ctrl;
	dmaengine_submit(dma_desc_tx);
	dma_async_issue_pending(tx->dma_chan);

	
	dma_desc_rx = dmaengine_prep_slave_sg(rx->dma_chan, sg_rx,
					sg_rx_itr - sg_rx, rx->dir, 0);
	if (dma_desc_rx < 0) {
		dev_err(ctrl->dev,
			"error dmaengine_prep_slave_sg rx:%ld\n",
						PTR_ERR(dma_desc_rx));
		ret = PTR_ERR(dma_desc_rx);
		goto dma_xfer_end;
	}

	dmaengine_submit(dma_desc_rx);
	dma_async_issue_pending(rx->dma_chan);

	
	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_RUN);
	if (ret) {
		dev_err(ctrl->dev, "transition to run state failed before DMA transaction :%d\n",
									ret);
		goto dma_xfer_end;
	}

	ret = i2c_msm_xfer_wait_for_completion(ctrl, &ctrl->xfer.complete);

dma_xfer_end:
	
	kfree(sg_tx);
	kfree(sg_rx);

	return ret;
}

static void i2c_msm_dma_free_channels(struct i2c_msm_ctrl *ctrl)
{
	int i;
	for (i = 0; i < I2C_MSM_DMA_CNT; ++i) {
		struct i2c_msm_dma_chan *chan = &ctrl->xfer.dma.chan[i];
		if (!chan->is_init)
			continue;

		dma_release_channel(chan->dma_chan);
		chan->is_init  = false;
		chan->dma_chan = NULL;
	}
	if (ctrl->xfer.dma.state > I2C_MSM_DMA_INIT_CORE)
		ctrl->xfer.dma.state = I2C_MSM_DMA_INIT_CORE;
}

static const char * const i2c_msm_dma_chan_name[] = {"tx", "rx"};

static int i2c_msm_dmaengine_dir[] = {
	DMA_MEM_TO_DEV, DMA_DEV_TO_MEM
};

static int i2c_msm_dma_init_channels(struct i2c_msm_ctrl *ctrl)
{
	int ret = 0;
	int i;
	
	for (i = 0; i < I2C_MSM_DMA_CNT; ++i) {
		struct dma_slave_config cfg = {0};
		struct i2c_msm_dma_chan *chan = &ctrl->xfer.dma.chan[i];
		if (chan->is_init)
			continue;

		chan->name     = i2c_msm_dma_chan_name[i];
		chan->dma_chan = dma_request_slave_channel(ctrl->dev,
								chan->name);
		if (!chan->dma_chan) {
			dev_err(ctrl->dev,
				"error dma_request_slave_channel(dev:%s chan:%s)\n",
				dev_name(ctrl->dev), chan->name);
			
			i2c_msm_dma_free_channels(ctrl);
			return -ENODEV;
		}

		chan->dir = cfg.direction = i2c_msm_dmaengine_dir[i];
		ret = dmaengine_slave_config(chan->dma_chan, &cfg);
		if (ret) {
			dev_err(ctrl->dev,
			"error:%d dmaengine_slave_config(chan:%s)\n",
						ret, chan->name);
			dma_release_channel(chan->dma_chan);
			chan->dma_chan = NULL;
			i2c_msm_dma_free_channels(ctrl);
			return ret;
		}
		chan->is_init = true;
	}
	ctrl->xfer.dma.state = I2C_MSM_DMA_INIT_CHAN;
	return 0;
}

static void i2c_msm_dma_teardown(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_dma *dma = &ctrl->xfer.dma;

	i2c_msm_dma_free_channels(ctrl);

	if (dma->state > I2C_MSM_DMA_INIT_NONE)
		dma_free_coherent(ctrl->dev, I2C_MSM_DMA_TAG_MEM_SZ,
				  dma->input_tag.vrtl_addr,
				  dma->input_tag.phy_addr);

	dma->state = I2C_MSM_DMA_INIT_NONE;
}

static int i2c_msm_dma_init(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_dma *dma = &ctrl->xfer.dma;
	u8             *tags_space_virt_addr;
	dma_addr_t      tags_space_phy_addr;

	
	if (dma->state > I2C_MSM_DMA_INIT_NONE)
		goto dma_core_is_init;

	tags_space_virt_addr = dma_alloc_coherent(
						ctrl->dev,
						I2C_MSM_DMA_TAG_MEM_SZ,
						&tags_space_phy_addr,
						GFP_KERNEL);
	if (!tags_space_virt_addr) {
		dev_err(ctrl->dev,
		  "error alloc %d bytes of DMAable memory for DMA tags space\n",
		  I2C_MSM_DMA_TAG_MEM_SZ);
		return -ENOMEM;
	}

	dma->input_tag.vrtl_addr  = tags_space_virt_addr;
	dma->input_tag.phy_addr   = tags_space_phy_addr;

	
	tags_space_virt_addr += I2C_MSM_TAG2_MAX_LEN;
	tags_space_phy_addr  += I2C_MSM_TAG2_MAX_LEN;
	dma->eot_n_flush_stop_tags.vrtl_addr = tags_space_virt_addr;
	dma->eot_n_flush_stop_tags.phy_addr  = tags_space_phy_addr;

	
	*((u16 *) dma->eot_n_flush_stop_tags.vrtl_addr) =
				QUP_TAG2_INPUT_EOT | (QUP_TAG2_FLUSH_STOP << 8);

	
	tags_space_virt_addr  += I2C_MSM_TAG2_MAX_LEN;
	tags_space_phy_addr   += I2C_MSM_TAG2_MAX_LEN;
	dma->tag_arr.vrtl_addr = tags_space_virt_addr;
	dma->tag_arr.phy_addr  = tags_space_phy_addr;

	dma->state = I2C_MSM_DMA_INIT_CORE;

dma_core_is_init:
	return i2c_msm_dma_init_channels(ctrl);
}

static int i2c_msm_dma_xfer(struct i2c_msm_ctrl *ctrl)
{
	int ret;
	ret = i2c_msm_dma_init(ctrl);
	if (ret) {
		dev_err(ctrl->dev, "DMA Init Failed: %d\n", ret);
		return ret;
	}

	
	ret = i2c_msm_dma_xfer_prepare(ctrl);
	if (ret < 0) {
		dev_err(ctrl->dev, "error on i2c_msm_dma_xfer_prepare():%d\n",
									ret);
		goto err_dma_xfer;
	}

	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_RESET);
	if (ret < 0)
		goto err_dma_xfer;

	
	i2c_msm_qup_xfer_init_reset_state(ctrl);

	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_RUN);
	if (ret < 0)
		goto err_dma_xfer;

	
	i2c_msm_qup_xfer_init_run_state(ctrl);

	
	ret = i2c_msm_dma_xfer_process(ctrl);
	if (ret)
		dev_err(ctrl->dev,
			"error i2c_msm_dma_xfer_process(n_bufs:%zu):%d\n",
			ctrl->xfer.dma.buf_arr_cnt, ret);

err_dma_xfer:
	i2c_msm_dma_xfer_unprepare(ctrl);
	return ret;
}

static bool i2c_msm_qup_slv_holds_bus(struct i2c_msm_ctrl *ctrl)
{
	u32 status = readl_relaxed(ctrl->rsrcs.base + QUP_I2C_STATUS);

	bool slv_holds_bus =	!(status & QUP_I2C_SDA) &&
				(status & QUP_BUS_ACTIVE) &&
				!(status & QUP_BUS_MASTER);
	if (slv_holds_bus)
		dev_info(ctrl->dev,
			"bus lines held low by a slave detected\n");

	return slv_holds_bus;
}

static int i2c_msm_qup_poll_bus_active_unset(struct i2c_msm_ctrl *ctrl)
{
	void __iomem *base    = ctrl->rsrcs.base;
	ulong timeout = jiffies + msecs_to_jiffies(I2C_MSM_MAX_POLL_MSEC);
	int    ret      = 0;
	size_t read_cnt = 0;

	do {
		if (!(readl_relaxed(base + QUP_I2C_STATUS) & QUP_BUS_ACTIVE))
			goto poll_active_end;
		++read_cnt;
	} while (time_before_eq(jiffies, timeout));

	ret = -EBUSY;

poll_active_end:
	
	i2c_msm_prof_evnt_add(ctrl, MSM_DBG, I2C_MSM_ACTV_END,
				ret, (ret ? 0 : (timeout - jiffies)), read_cnt);

	return ret;
}

static void i2c_msm_clk_path_vote(struct i2c_msm_ctrl *ctrl)
{
	i2c_msm_clk_path_init(ctrl);

	if (ctrl->rsrcs.clk_path_vote.client_hdl)
		msm_bus_scale_client_update_request(
					ctrl->rsrcs.clk_path_vote.client_hdl,
					I2C_MSM_CLK_PATH_RESUME_VEC);
}

static void i2c_msm_clk_path_unvote(struct i2c_msm_ctrl *ctrl)
{
	if (ctrl->rsrcs.clk_path_vote.client_hdl)
		msm_bus_scale_client_update_request(
					ctrl->rsrcs.clk_path_vote.client_hdl,
					I2C_MSM_CLK_PATH_SUSPEND_VEC);
}

static void i2c_msm_clk_path_teardown(struct i2c_msm_ctrl *ctrl)
{
	if (ctrl->rsrcs.clk_path_vote.client_hdl) {
		msm_bus_scale_unregister_client(
					ctrl->rsrcs.clk_path_vote.client_hdl);
		ctrl->rsrcs.clk_path_vote.client_hdl = 0;
	}
}

static int i2c_msm_clk_path_init_structs(struct i2c_msm_ctrl *ctrl)
{
	struct msm_bus_vectors *paths    = NULL;
	struct msm_bus_paths   *usecases = NULL;

	i2c_msm_dbg(ctrl, MSM_PROF, "initializes path clock voting structs");

	paths = devm_kzalloc(ctrl->dev, sizeof(*paths) * 2, GFP_KERNEL);
	if (!paths) {
		dev_err(ctrl->dev,
			"error msm_bus_paths.paths memory allocation failed\n");
		return -ENOMEM;
	}

	usecases = devm_kzalloc(ctrl->dev, sizeof(*usecases) * 2, GFP_KERNEL);
	if (!usecases) {
		dev_err(ctrl->dev,
		"error  msm_bus_scale_pdata.usecases memory allocation failed\n");
		goto path_init_err;
	}

	ctrl->rsrcs.clk_path_vote.pdata = devm_kzalloc(ctrl->dev,
				       sizeof(*ctrl->rsrcs.clk_path_vote.pdata),
				       GFP_KERNEL);
	if (!ctrl->rsrcs.clk_path_vote.pdata) {
		dev_err(ctrl->dev,
			"error  msm_bus_scale_pdata memory allocation failed\n");
		goto path_init_err;
	}

	paths[I2C_MSM_CLK_PATH_SUSPEND_VEC] = (struct msm_bus_vectors) {
		.src = ctrl->rsrcs.clk_path_vote.mstr_id,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	};

	paths[I2C_MSM_CLK_PATH_RESUME_VEC]  = (struct msm_bus_vectors) {
		.src = ctrl->rsrcs.clk_path_vote.mstr_id,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = I2C_MSM_CLK_PATH_AVRG_BW(ctrl),
		.ib  = I2C_MSM_CLK_PATH_BRST_BW(ctrl),
	};

	usecases[I2C_MSM_CLK_PATH_SUSPEND_VEC] = (struct msm_bus_paths) {
		.num_paths = 1,
		.vectors   = &paths[I2C_MSM_CLK_PATH_SUSPEND_VEC],
	};

	usecases[I2C_MSM_CLK_PATH_RESUME_VEC] = (struct msm_bus_paths) {
		.num_paths = 1,
		.vectors   = &paths[I2C_MSM_CLK_PATH_RESUME_VEC],
	};

	*ctrl->rsrcs.clk_path_vote.pdata = (struct msm_bus_scale_pdata) {
		.usecase      = usecases,
		.num_usecases = 2,
		.name         = dev_name(ctrl->dev),
	};

	return 0;

path_init_err:
	devm_kfree(ctrl->dev, paths);
	devm_kfree(ctrl->dev, usecases);
	devm_kfree(ctrl->dev, ctrl->rsrcs.clk_path_vote.pdata);
	ctrl->rsrcs.clk_path_vote.pdata = NULL;
	return -ENOMEM;
}

static int i2c_msm_clk_path_postponed_register(struct i2c_msm_ctrl *ctrl)
{
	ctrl->rsrcs.clk_path_vote.client_hdl =
		msm_bus_scale_register_client(ctrl->rsrcs.clk_path_vote.pdata);

	if (ctrl->rsrcs.clk_path_vote.client_hdl) {
		if (ctrl->rsrcs.clk_path_vote.reg_err) {
			
			ctrl->rsrcs.clk_path_vote.reg_err = false;
			dev_err(ctrl->dev,
				"msm_bus_scale_register_client(mstr-id:%d):0x%x (ok)",
				ctrl->rsrcs.clk_path_vote.mstr_id,
				ctrl->rsrcs.clk_path_vote.client_hdl);
		}
	} else {
		
		if (!ctrl->rsrcs.clk_path_vote.reg_err) {
			ctrl->rsrcs.clk_path_vote.reg_err = true;

			dev_info(ctrl->dev,
				"msm_bus_scale_register_client(mstr-id:%d):0 (not a problem)",
				ctrl->rsrcs.clk_path_vote.mstr_id);
		}
	}

	return ctrl->rsrcs.clk_path_vote.client_hdl ? 0 : -EAGAIN;
}

static void i2c_msm_clk_path_init(struct i2c_msm_ctrl *ctrl)
{
	if (!ctrl->rsrcs.clk_path_vote.mstr_id ||
		ctrl->rsrcs.clk_path_vote.client_hdl)
		return;

	
	if (!ctrl->rsrcs.clk_path_vote.pdata &&
					i2c_msm_clk_path_init_structs(ctrl)) {
		ctrl->rsrcs.clk_path_vote.mstr_id = 0;
		return;
	};

	
	if (i2c_msm_clk_path_postponed_register(ctrl))
		return;
}

static irqreturn_t i2c_msm_qup_isr(int irq, void *devid)
{
	struct i2c_msm_ctrl *ctrl = devid;
	void __iomem        *base = ctrl->rsrcs.base;
	struct i2c_msm_xfer *xfer = &ctrl->xfer;
	struct i2c_msm_xfer_mode_blk *blk = &ctrl->xfer.blk;
	u32  i2c_status = 0;
	u32  err_flags  = 0;
	u32  qup_op     = 0;
	u32  clr_flds   = 0;
	bool log_event       = false;
	bool signal_complete = false;
	bool need_wmb        = false;

	i2c_msm_prof_evnt_add(ctrl, MSM_PROF, I2C_MSM_IRQ_BGN, irq, 0, 0);

	if (!atomic_read(&ctrl->xfer.is_active)) {
		dev_info(ctrl->dev, "irq:%d when no active transfer\n", irq);
		return IRQ_HANDLED;
	}

	i2c_status  = readl_relaxed(base + QUP_I2C_STATUS);
	err_flags   = readl_relaxed(base + QUP_ERROR_FLAGS);
	qup_op      = readl_relaxed(base + QUP_OPERATIONAL);

	if (i2c_status & QUP_MSTR_STTS_ERR_MASK) {
		signal_complete = true;
		log_event       = true;
		if (i2c_status & QUP_ARB_LOST)
			ctrl->xfer.err = I2C_MSM_ERR_ARB_LOST;

		if (i2c_status & QUP_BUS_ERROR)
			ctrl->xfer.err = I2C_MSM_ERR_BUS_ERR;

		if (i2c_status & QUP_PACKET_NACKED)
			ctrl->xfer.err = I2C_MSM_ERR_NACK;
	}

	
	if (err_flags & QUP_ERR_FLGS_MASK)
		ctrl->xfer.err = I2C_MSM_ERR_OVR_UNDR_RUN;

	
	if (ctrl->xfer.err && ctrl->dbgfs.dbg_lvl >= MSM_DBG)
		i2c_msm_dbg_qup_reg_dump(ctrl);

	
	clr_flds = i2c_status & QUP_MSTR_STTS_ERR_MASK;
	if (clr_flds) {
		writel_relaxed(clr_flds, base + QUP_I2C_STATUS);
		need_wmb = true;
	}

	clr_flds = err_flags & QUP_ERR_FLGS_MASK;
	if (clr_flds) {
		writel_relaxed(clr_flds,  base + QUP_ERROR_FLAGS);
		need_wmb = true;
	}

	clr_flds = qup_op & (QUP_OUTPUT_SERVICE_FLAG | QUP_INPUT_SERVICE_FLAG);
	if (clr_flds) {
		writel_relaxed(clr_flds, base + QUP_OPERATIONAL);
		need_wmb = true;
	}

	if (need_wmb)
		wmb();

	
	if (ctrl->xfer.err) {
		
		if (ctrl->xfer.mode_id == I2C_MSM_XFER_MODE_DMA) {
			writel_relaxed(QUP_I2C_FLUSH, ctrl->rsrcs.base
								+ QUP_STATE);
			/*
			 * Ensure that QUP_I2C_FLUSH is written before
			 * State reset
			 */
			wmb();
		}

		writel_relaxed(QUP_STATE_RESET, ctrl->rsrcs.base + QUP_STATE);

		signal_complete = true;
		log_event       = true;
		goto isr_end;
	}

	
	if (xfer->mode_id == I2C_MSM_XFER_MODE_BLOCK) {
		
		if (qup_op & QUP_OUTPUT_SERVICE_FLAG) {
			log_event = true;
			if (qup_op & QUP_OUT_BLOCK_WRITE_REQ)
				complete(&blk->wait_tx_blk);

			if ((qup_op & blk->complete_mask)
					== blk->complete_mask) {
				log_event       = true;
				signal_complete = true;
			}
		}
		
		if (qup_op & QUP_INPUT_SERVICE_FLAG) {
			log_event = true;
			complete(&blk->wait_rx_blk);
		}
	} else {
		
		if (qup_op & QUP_MAX_INPUT_DONE_FLAG) {
			log_event = true;
			if (ctrl->xfer.last_is_rx)
				signal_complete = true;
		}
		if (qup_op & (QUP_OUTPUT_SERVICE_FLAG |
						QUP_MAX_OUTPUT_DONE_FLAG)) {
			log_event = true;
			if (!ctrl->xfer.last_is_rx)
				signal_complete = true;
		}
	}

isr_end:
	if (ctrl->xfer.err || (ctrl->dbgfs.dbg_lvl >= MSM_DBG))
		i2c_msm_dbg_dump_diag(ctrl, true, i2c_status, qup_op);

	if (log_event || (ctrl->dbgfs.dbg_lvl >= MSM_DBG))
		i2c_msm_prof_evnt_add(ctrl, MSM_PROF,
					I2C_MSM_IRQ_END,
					i2c_status, qup_op, err_flags);

	if (signal_complete)
		complete(&ctrl->xfer.complete);

	return IRQ_HANDLED;
}

static void i2x_msm_blk_free_cache(struct i2c_msm_ctrl *ctrl)
{
	kfree(ctrl->xfer.blk.tx_cache);
	kfree(ctrl->xfer.blk.rx_cache);
}

static void i2c_msm_qup_init(struct i2c_msm_ctrl *ctrl)
{
	u32 state;
	void __iomem *base = ctrl->rsrcs.base;

	i2c_msm_prof_evnt_add(ctrl, MSM_PROF, I2C_MSM_PROF_RESET, 0, 0, 0);

	i2c_msm_qup_sw_reset(ctrl);
	i2c_msm_qup_state_set(ctrl, QUP_STATE_RESET);

	writel_relaxed(QUP_N_VAL | QUP_MINI_CORE_I2C_VAL, base + QUP_CONFIG);

	writel_relaxed(QUP_OUTPUT_OVER_RUN_ERR_EN | QUP_INPUT_UNDER_RUN_ERR_EN
		     | QUP_OUTPUT_UNDER_RUN_ERR_EN | QUP_INPUT_OVER_RUN_ERR_EN,
					base + QUP_ERROR_FLAGS_EN);

	writel_relaxed(QUP_INPUT_SERVICE_MASK | QUP_OUTPUT_SERVICE_MASK,
					base + QUP_OPERATIONAL_MASK);

	writel_relaxed(QUP_EN_VERSION_TWO_TAG, base + QUP_I2C_MASTER_CONFIG);

	i2c_msm_qup_fifo_calc_size(ctrl);
	/*
	 * Ensure that QUP configuration is written and that fifo size if read
	 * before leaving this function
	 */
	mb();

	state = readl_relaxed(base + QUP_STATE);

	if (!(state & QUP_I2C_MAST_GEN))
		dev_err(ctrl->dev,
			"error on verifying HW support (I2C_MAST_GEN=0)\n");
}

static int qup_i2c_try_recover_bus_busy(struct i2c_msm_ctrl *ctrl)
{
	int ret;
	ulong min_sleep_usec;

	
	i2c_msm_qup_init(ctrl);

	
	ret = i2c_msm_qup_state_set(ctrl, QUP_STATE_RUN);
	if (ret < 0) {
		dev_err(ctrl->dev, "error: bus clear fail to set run state\n");
		return ret;
	}

	i2c_msm_qup_xfer_init_run_state(ctrl);

	writel_relaxed(0x1, ctrl->rsrcs.base + QUP_I2C_MASTER_BUS_CLR);

	min_sleep_usec =
	  max_t(ulong, (9 * 10 * USEC_PER_SEC) / ctrl->rsrcs.clk_freq_out, 100);

	usleep_range(min_sleep_usec, min_sleep_usec * 10);
	return ret;
}

static int qup_i2c_recover_bus_busy(struct i2c_msm_ctrl *ctrl)
{
	u32 bus_clr, bus_active, status;
	int retry = 0;
	dev_info(ctrl->dev, "Executing bus recovery procedure (9 clk pulse)\n");

	do {
		qup_i2c_try_recover_bus_busy(ctrl);
		bus_clr    = readl_relaxed(ctrl->rsrcs.base +
							QUP_I2C_MASTER_BUS_CLR);
		status     = readl_relaxed(ctrl->rsrcs.base + QUP_I2C_STATUS);
		bus_active = status & I2C_STATUS_BUS_ACTIVE;
		if (++retry >= I2C_QUP_MAX_BUS_RECOVERY_RETRY)
			break;
	} while (bus_clr || bus_active);

	dev_info(ctrl->dev, "Bus recovery %s after %d retries\n",
		(bus_clr || bus_active) ? "fail" : "success", retry);
	return 0;
}

static int i2c_msm_qup_post_xfer(struct i2c_msm_ctrl *ctrl, int err)
{
	
	if (i2c_msm_qup_poll_bus_active_unset(ctrl)) {
		if ((ctrl->xfer.err == I2C_MSM_ERR_ARB_LOST) ||
		    (ctrl->xfer.err == I2C_MSM_ERR_BUS_ERR)  ||
		    (ctrl->xfer.err == I2C_MSM_ERR_TIMEOUT)) {
			if (i2c_msm_qup_slv_holds_bus(ctrl))
				qup_i2c_recover_bus_busy(ctrl);

			
			if (!err)
				err = -EIO;
		}
	}

	disable_irq(ctrl->rsrcs.irq);

	if (ctrl->xfer.err & I2C_MSM_ERR_TIMEOUT) {
		
		if (ctrl->xfer.mode_id == I2C_MSM_XFER_MODE_DMA)
			writel_relaxed(QUP_I2C_FLUSH, ctrl->rsrcs.base
								+ QUP_STATE);

		
		i2c_msm_qup_state_set(ctrl, QUP_STATE_RESET);
		err = -ETIMEDOUT;
	} else if (ctrl->xfer.err == I2C_MSM_ERR_NACK) {
		err = -ENOTCONN;
	}

	return err;
}

static enum i2c_msm_xfer_mode_id
i2c_msm_qup_choose_mode(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_mode_fifo *fifo = &ctrl->xfer.fifo;
	struct i2c_msm_xfer           *xfer = &ctrl->xfer;
	size_t rx_cnt_sum = xfer->rx_cnt + xfer->rx_ovrhd_cnt;
	size_t tx_cnt_sum = xfer->tx_cnt + xfer->tx_ovrhd_cnt;


	if (ctrl->dbgfs.force_xfer_mode != I2C_MSM_XFER_MODE_NONE)
		return ctrl->dbgfs.force_xfer_mode;

	if (((rx_cnt_sum < fifo->input_fifo_sz) &&
		(tx_cnt_sum < fifo->output_fifo_sz)))
		return I2C_MSM_XFER_MODE_FIFO;

	if (ctrl->rsrcs.disable_dma)
		return I2C_MSM_XFER_MODE_BLOCK;

	return I2C_MSM_XFER_MODE_DMA;
}

static void i2c_msm_xfer_calc_timeout(struct i2c_msm_ctrl *ctrl)
{
	size_t byte_cnt = ctrl->xfer.rx_cnt + ctrl->xfer.tx_cnt;
	size_t bit_cnt  = byte_cnt * 9;
	size_t bit_usec = (bit_cnt * USEC_PER_SEC) / ctrl->rsrcs.clk_freq_out;
	size_t loging_ovrhd_coef = ctrl->dbgfs.dbg_lvl + 1;
	size_t safety_coef   = I2C_MSM_TIMEOUT_SAFTY_COEF * loging_ovrhd_coef;
	size_t xfer_max_usec = (bit_usec * safety_coef) +
						I2C_MSM_TIMEOUT_MIN_USEC;

	ctrl->xfer.timeout = usecs_to_jiffies(xfer_max_usec);
}

static int i2c_msm_xfer_wait_for_completion(struct i2c_msm_ctrl *ctrl,
						struct completion *complete)
{
	struct i2c_msm_xfer *xfer = &ctrl->xfer;
	long  time_left;
	int   ret = 0;

	time_left = wait_for_completion_timeout(complete, xfer->timeout);
	if (!time_left) {
		xfer->err = I2C_MSM_ERR_TIMEOUT;
		i2c_msm_dbg_dump_diag(ctrl, false, 0, 0);
		ret = -EIO;
		i2c_msm_prof_evnt_add(ctrl, MSM_ERR, I2C_MSM_COMPLT_FL,
						xfer->timeout, time_left, 0);
	} else {
		
		if (xfer->err)
			ret = -(xfer->err);
		i2c_msm_prof_evnt_add(ctrl, MSM_DBG, I2C_MSM_COMPLT_OK,
					xfer->timeout, time_left, 0);
	}

	return ret;
}

static u16 i2c_msm_slv_rd_wr_addr(u16 slv_addr, bool is_rx)
{
	return (slv_addr << 1) | (is_rx ? 0x1 : 0x0);
}

static bool i2c_msm_xfer_msg_is_last(struct i2c_msm_ctrl *ctrl)
{
	return ctrl->xfer.cur_buf.msg_idx >= (ctrl->xfer.msg_cnt - 1);
}

static bool i2c_msm_xfer_buf_is_last(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_buf *cur_buf = &ctrl->xfer.cur_buf;
	struct i2c_msg *cur_msg = ctrl->xfer.msgs + cur_buf->msg_idx;

	return i2c_msm_xfer_msg_is_last(ctrl) &&
		((cur_buf->byte_idx + QUP_MAX_BUF_SZ) >= cur_msg->len);
}

static void i2c_msm_xfer_create_cur_tag(struct i2c_msm_ctrl *ctrl,
								bool start_req)
{
	struct i2c_msm_xfer_buf *cur_buf = &ctrl->xfer.cur_buf;

	cur_buf->out_tag = i2c_msm_tag_create(start_req, cur_buf->is_last,
					cur_buf->is_rx, cur_buf->len,
					cur_buf->slv_addr);

	cur_buf->in_tag.len = cur_buf->is_rx ? QUP_BUF_OVERHD_BC : 0;
}

static bool i2c_msm_xfer_next_buf(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer_buf *cur_buf = &ctrl->xfer.cur_buf;
	struct i2c_msg          *cur_msg = ctrl->xfer.msgs + cur_buf->msg_idx;
	int bc_rem = cur_msg->len - cur_buf->end_idx;

	if (cur_buf->is_init && cur_buf->end_idx && bc_rem) {
		

		cur_buf->byte_idx  = cur_buf->end_idx;
		cur_buf->is_last   = i2c_msm_xfer_buf_is_last(ctrl);
		cur_buf->len       = min_t(int, bc_rem, QUP_MAX_BUF_SZ);
		cur_buf->end_idx  += cur_buf->len;

		
		i2c_msm_xfer_create_cur_tag(ctrl, false);
	} else {
		
		if (cur_buf->is_init) {
			if (i2c_msm_xfer_msg_is_last(ctrl)) {
				return false;
			} else {
				++cur_buf->msg_idx;
				++cur_msg;
			}
		} else {
			cur_buf->is_init = true;
		}
		cur_buf->byte_idx  = 0;
		cur_buf->is_last   = i2c_msm_xfer_buf_is_last(ctrl);
		cur_buf->len       = min_t(int, cur_msg->len, QUP_MAX_BUF_SZ);
		cur_buf->is_rx     = (cur_msg->flags & I2C_M_RD);
		cur_buf->end_idx   = cur_buf->len;
		cur_buf->slv_addr  = i2c_msm_slv_rd_wr_addr(cur_msg->addr,
								cur_buf->is_rx);
		i2c_msm_xfer_create_cur_tag(ctrl, true);
	}
	i2c_msm_prof_evnt_add(ctrl, MSM_DBG, I2C_MSM_NEXT_BUF, cur_buf->msg_idx,
							cur_buf->byte_idx, 0);
	return  true;
}

static void i2c_msm_pm_clk_disable_unprepare(struct i2c_msm_ctrl *ctrl)
{
	clk_disable_unprepare(ctrl->rsrcs.core_clk);
	clk_disable_unprepare(ctrl->rsrcs.iface_clk);
}

static int i2c_msm_pm_clk_prepare_enable(struct i2c_msm_ctrl *ctrl)
{
	int ret;
	ret = clk_prepare_enable(ctrl->rsrcs.iface_clk);
	if (ret) {
		dev_err(ctrl->dev,
			"error on clk_prepare_enable(iface_clk):%d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(ctrl->rsrcs.core_clk);
	if (ret) {
		clk_disable_unprepare(ctrl->rsrcs.iface_clk);
		dev_err(ctrl->dev,
			"error clk_prepare_enable(core_clk):%d\n", ret);
	}
	return ret;
}

static int i2c_msm_pm_xfer_start(struct i2c_msm_ctrl *ctrl)
{
	int ret;
	struct i2c_msm_xfer *xfer = &ctrl->xfer;
	mutex_lock(&ctrl->xfer.mtx);

	
	if (ctrl->pwr_state == I2C_MSM_PM_SYS_SUSPENDED) {
		struct i2c_msg *msgs = xfer->msgs + xfer->cur_buf.msg_idx;
		dev_err(ctrl->dev,
				"slave:0x%x is calling xfer when system is suspended\n",
				msgs->addr);
		mutex_unlock(&ctrl->xfer.mtx);
		return -EIO;
	}

	pm_runtime_get_sync(ctrl->dev);
	if (ctrl->pwr_state != I2C_MSM_PM_RT_ACTIVE) {
		dev_info(ctrl->dev, "Runtime PM-callback was not invoked.\n");
		i2c_msm_pm_resume(ctrl->dev);
	}

	ret = i2c_msm_pm_clk_prepare_enable(ctrl);
	if (ret) {
		mutex_unlock(&ctrl->xfer.mtx);
		return ret;
	}
	i2c_msm_qup_init(ctrl);

	
	atomic_set(&ctrl->xfer.is_active, 1);

	enable_irq(ctrl->rsrcs.irq);
	return 0;
}

static void i2c_msm_pm_xfer_end(struct i2c_msm_ctrl *ctrl)
{

	atomic_set(&ctrl->xfer.is_active, 0);

	if (ctrl->xfer.mode_id == I2C_MSM_XFER_MODE_DMA)
		i2c_msm_dma_free_channels(ctrl);

	i2c_msm_pm_clk_disable_unprepare(ctrl);
	if (pm_runtime_enabled(ctrl->dev)) {
		pm_runtime_mark_last_busy(ctrl->dev);
		pm_runtime_put_autosuspend(ctrl->dev);
	} else {
		i2c_msm_pm_suspend(ctrl->dev);
	}
	mutex_unlock(&ctrl->xfer.mtx);
}

static void i2c_msm_xfer_scan(struct i2c_msm_ctrl *ctrl)
{
	struct i2c_msm_xfer     *xfer      = &ctrl->xfer;
	struct i2c_msm_xfer_buf *cur_buf   = &xfer->cur_buf;

	while (i2c_msm_xfer_next_buf(ctrl)) {

		if (cur_buf->is_rx)
			xfer->rx_cnt += cur_buf->len;
		else
			xfer->tx_cnt += cur_buf->len;

		xfer->rx_ovrhd_cnt += cur_buf->in_tag.len;
		xfer->tx_ovrhd_cnt += cur_buf->out_tag.len;

		if (i2c_msm_xfer_msg_is_last(ctrl))
			xfer->last_is_rx = cur_buf->is_rx;
	}
	xfer->cur_buf = (struct i2c_msm_xfer_buf){0};
}

static int
i2c_msm_frmwrk_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[], int num)
{
	int ret = 0;
	struct i2c_msm_ctrl      *ctrl = i2c_get_adapdata(adap);
	struct i2c_msm_xfer      *xfer = &ctrl->xfer;

	if (IS_ERR_OR_NULL(msgs)) {
		dev_err(ctrl->dev, " error on msgs Accessing invalid  pointer location\n");
		return PTR_ERR(msgs);
	}

	ret = i2c_msm_pm_xfer_start(ctrl);
	if (ret)
		return ret;

	
	xfer->msgs         = msgs;
	xfer->msg_cnt      = num;
	xfer->mode_id      = I2C_MSM_XFER_MODE_NONE;
	xfer->err          = 0;
	xfer->rx_cnt       = 0;
	xfer->tx_cnt       = 0;
	xfer->rx_ovrhd_cnt = 0;
	xfer->tx_ovrhd_cnt = 0;
	atomic_set(&xfer->event_cnt, 0);
	init_completion(&xfer->complete);
	xfer->cur_buf.is_init = false;
	xfer->cur_buf.msg_idx = 0;

	i2c_msm_prof_evnt_add(ctrl, MSM_PROF, I2C_MSM_XFER_BEG, num,
								msgs->addr, 0);

	i2c_msm_xfer_scan(ctrl);
	i2c_msm_xfer_calc_timeout(ctrl);
	xfer->mode_id = i2c_msm_qup_choose_mode(ctrl);

	dev_dbg(ctrl->dev, "xfer() mode:%d msg_cnt:%d rx_cbt:%zu tx_cnt:%zu\n",
		xfer->mode_id, xfer->msg_cnt, xfer->rx_cnt, xfer->tx_cnt);

	switch (xfer->mode_id) {
	case I2C_MSM_XFER_MODE_FIFO:
		ret = i2c_msm_fifo_xfer(ctrl);
		break;
	case I2C_MSM_XFER_MODE_BLOCK:
		ret = i2c_msm_blk_xfer(ctrl);
		break;
	case I2C_MSM_XFER_MODE_DMA:
		ret = i2c_msm_dma_xfer(ctrl);
		break;
	default:
		ret = -EINTR;
	};

	i2c_msm_prof_evnt_add(ctrl, MSM_PROF, I2C_MSM_SCAN_SUM,
		((xfer->rx_cnt & 0xff) | ((xfer->rx_ovrhd_cnt & 0xff) << 16)),
		((xfer->tx_cnt & 0xff) | ((xfer->tx_ovrhd_cnt & 0xff) << 16)),
		((ctrl->xfer.timeout & 0xfff) | ((xfer->mode_id & 0xf) << 24)));

	ret = i2c_msm_qup_post_xfer(ctrl, ret);
	
	if (!ret)
		ret = xfer->cur_buf.msg_idx + 1;

	i2c_msm_prof_evnt_add(ctrl, MSM_PROF, I2C_MSM_XFER_END, ret, xfer->err,
						xfer->cur_buf.msg_idx + 1);
	
	if (xfer->err || (ctrl->dbgfs.dbg_lvl >= MSM_PROF))
		i2c_msm_prof_evnt_dump(ctrl);

	i2c_msm_pm_xfer_end(ctrl);
	return ret;
}

enum i2c_msm_dt_entry_status {
	DT_REQ,  
	DT_SGST, 
	DT_OPT,  
};

enum i2c_msm_dt_entry_type {
	DT_U32,
	DT_BOOL,
	DT_ID,   
};

struct i2c_msm_dt_to_pdata_map {
	const char                  *dt_name;
	void                        *ptr_data;
	enum i2c_msm_dt_entry_status status;
	enum i2c_msm_dt_entry_type   type;
	int                          default_val;
};

static int i2c_msm_dt_to_pdata_populate(struct i2c_msm_ctrl *ctrl,
					struct platform_device *pdev,
					struct i2c_msm_dt_to_pdata_map *itr)
{
	int  ret, err = 0;
	struct device_node *node = pdev->dev.of_node;

	for (; itr->dt_name ; ++itr) {
		switch (itr->type) {
		case DT_U32:
			ret = of_property_read_u32(node, itr->dt_name,
							 (u32 *) itr->ptr_data);
			break;
		case DT_BOOL:
			*((bool *) itr->ptr_data) =
				of_property_read_bool(node, itr->dt_name);
			ret = 0;
			break;
		case DT_ID:
			ret = of_alias_get_id(node, itr->dt_name);
			if (ret >= 0) {
				*((int *) itr->ptr_data) = ret;
				ret = 0;
			}
			break;
		default:
			dev_err(ctrl->dev,
				"error %d is of unknown DT entry type\n",
				itr->type);
			ret = -EBADE;
		}

		i2c_msm_dbg(ctrl, MSM_PROF, "DT entry ret:%d name:%s val:%d",
				ret, itr->dt_name, *((int *)itr->ptr_data));

		if (ret) {
			*((int *)itr->ptr_data) = itr->default_val;

			if (itr->status < DT_OPT) {
				dev_err(ctrl->dev,
					"error Missing '%s' DT entry\n",
					itr->dt_name);

				
				if (itr->status == DT_REQ && !err)
					err = ret;
			}
		}
	}

	return err;
}


static int i2c_msm_rsrcs_process_dt(struct i2c_msm_ctrl *ctrl,
					struct platform_device *pdev)
{
	u32 fs_clk_div, ht_clk_div, noise_rjct_scl, noise_rjct_sda;
	int ret;

	struct i2c_msm_dt_to_pdata_map map[] = {
	{"i2c",				&pdev->id,	DT_REQ,  DT_ID,  -1},
	{"qcom,clk-freq-out",		&ctrl->rsrcs.clk_freq_out,
							DT_REQ,  DT_U32,  0},
	{"qcom,clk-freq-in",		&ctrl->rsrcs.clk_freq_in,
							DT_REQ,  DT_U32,  0},
	{"qcom,disable-dma",		&(ctrl->rsrcs.disable_dma),
							DT_OPT,  DT_BOOL, 0},
	{"qcom,master-id",		&(ctrl->rsrcs.clk_path_vote.mstr_id),
							DT_SGST, DT_U32,  0},
	{"qcom,noise-rjct-scl",		&noise_rjct_scl,
							DT_OPT,  DT_U32,  0},
	{"qcom,noise-rjct-sda",		&noise_rjct_sda,
							DT_OPT,  DT_U32,  0},
	{"qcom,high-time-clk-div",	&ht_clk_div,
							DT_OPT,  DT_U32,  0},
	{"qcom,fs-clk-div",		&fs_clk_div,
							DT_OPT,  DT_U32,  0},
	{"qcom,nack-as-normal",		&(ctrl->rsrcs.nack_as_normal),
							DT_OPT,  DT_U32,  0},
	{NULL,  NULL,					0,       0,       0},
	};

	ret = i2c_msm_dt_to_pdata_populate(ctrl, pdev, map);
	if (ret)
		return ret;

	
	return i2c_msm_set_mstr_clk_ctl(ctrl, fs_clk_div, ht_clk_div,
						noise_rjct_scl, noise_rjct_sda);
}

static int i2c_msm_rsrcs_mem_init(struct platform_device *pdev,
						struct i2c_msm_ctrl *ctrl)
{
	struct resource *mem_region;

	ctrl->rsrcs.mem = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"qup_phys_addr");
	if (!ctrl->rsrcs.mem) {
		dev_err(ctrl->dev, "error Missing 'qup_phys_addr' resource\n");
		return -ENODEV;
	}

	mem_region = request_mem_region(ctrl->rsrcs.mem->start,
					resource_size(ctrl->rsrcs.mem),
					pdev->name);
	if (!mem_region) {
		dev_err(ctrl->dev,
			"QUP physical memory region already claimed\n");
		return -EBUSY;
	}

	ctrl->rsrcs.base = devm_ioremap(ctrl->dev, ctrl->rsrcs.mem->start,
				   resource_size(ctrl->rsrcs.mem));
	if (!ctrl->rsrcs.base) {
		dev_err(ctrl->dev,
			"error failed ioremap(base:0x%llx size:0x%llx\n)",
			(u64) ctrl->rsrcs.mem->start,
			(u64) resource_size(ctrl->rsrcs.mem));
		release_mem_region(ctrl->rsrcs.mem->start,
						resource_size(ctrl->rsrcs.mem));
		return -ENOMEM;
	}

	return 0;
}

static void i2c_msm_rsrcs_mem_teardown(struct i2c_msm_ctrl *ctrl)
{
	release_mem_region(ctrl->rsrcs.mem->start,
						resource_size(ctrl->rsrcs.mem));
}

static int i2c_msm_rsrcs_irq_init(struct platform_device *pdev,
						struct i2c_msm_ctrl *ctrl)
{
	int ret, irq;

	irq = platform_get_irq_byname(pdev, "qup_irq");
	if (irq < 0) {
		dev_err(ctrl->dev, "error reading irq resource\n");
		return irq;
	}

	ret = request_irq(irq, i2c_msm_qup_isr, IRQF_TRIGGER_HIGH,
						"i2c-msm-v2-irq", ctrl);
	if (ret) {
		dev_err(ctrl->dev, "error request_irq(irq_num:%d ) ret:%d\n",
								irq, ret);
		return ret;
	}

	disable_irq(irq);
	ctrl->rsrcs.irq = irq;
	return 0;
}

static void i2c_msm_rsrcs_irq_teardown(struct i2c_msm_ctrl *ctrl)
{
	free_irq(ctrl->rsrcs.irq, ctrl);
}


static struct pinctrl_state *
i2c_msm_rsrcs_gpio_get_state(struct i2c_msm_ctrl *ctrl, const char *name)
{
	struct pinctrl_state *pin_state
			= pinctrl_lookup_state(ctrl->rsrcs.pinctrl, name);

	if (IS_ERR_OR_NULL(pin_state))
		dev_info(ctrl->dev, "note pinctrl_lookup_state(%s) err:%ld\n",
						name, PTR_ERR(pin_state));
	return pin_state;
}

static int i2c_msm_rsrcs_gpio_pinctrl_init(struct i2c_msm_ctrl *ctrl)
{
	ctrl->rsrcs.pinctrl = devm_pinctrl_get(ctrl->dev);
	if (IS_ERR_OR_NULL(ctrl->rsrcs.pinctrl)) {
		dev_err(ctrl->dev, "error devm_pinctrl_get() failed err:%ld\n",
				PTR_ERR(ctrl->rsrcs.pinctrl));
		return PTR_ERR(ctrl->rsrcs.pinctrl);
	}

	ctrl->rsrcs.gpio_state_active =
		i2c_msm_rsrcs_gpio_get_state(ctrl, I2C_MSM_PINCTRL_ACTIVE);

	ctrl->rsrcs.gpio_state_suspend =
		i2c_msm_rsrcs_gpio_get_state(ctrl, I2C_MSM_PINCTRL_SUSPEND);

	return 0;
}

static void i2c_msm_pm_pinctrl_state(struct i2c_msm_ctrl *ctrl,
				bool runtime_active)
{
	struct pinctrl_state *pins_state;
	const char           *pins_state_name;

	if (runtime_active) {
		pins_state      = ctrl->rsrcs.gpio_state_active;
		pins_state_name = I2C_MSM_PINCTRL_ACTIVE;
	} else {
		pins_state      = ctrl->rsrcs.gpio_state_suspend;
		pins_state_name = I2C_MSM_PINCTRL_SUSPEND;
	}

	if (!IS_ERR_OR_NULL(pins_state)) {
		int ret = pinctrl_select_state(ctrl->rsrcs.pinctrl, pins_state);
		if (ret)
			dev_err(ctrl->dev,
			"error pinctrl_select_state(%s) err:%d\n",
			pins_state_name, ret);
	} else {
		dev_err(ctrl->dev,
			"error pinctrl state-name:'%s' is not configured\n",
			pins_state_name);
	}
}

static int i2c_msm_rsrcs_clk_init(struct i2c_msm_ctrl *ctrl)
{
	int ret = 0;

	if ((ctrl->rsrcs.clk_freq_out <= 0) ||
	    (ctrl->rsrcs.clk_freq_out > I2C_MSM_CLK_FAST_PLUS_FREQ)) {
		dev_err(ctrl->dev,
			"error clock frequency %dKHZ is not supported\n",
			(ctrl->rsrcs.clk_freq_out / 1000));
		return -EIO;
	}

	ctrl->rsrcs.core_clk = clk_get(ctrl->dev, "core_clk");
	if (IS_ERR(ctrl->rsrcs.core_clk)) {
		ret = PTR_ERR(ctrl->rsrcs.core_clk);
		dev_err(ctrl->dev, "error on clk_get(core_clk):%d\n", ret);
		return ret;
	}

	ret = clk_set_rate(ctrl->rsrcs.core_clk, ctrl->rsrcs.clk_freq_in);
	if (ret) {
		dev_err(ctrl->dev, "error on clk_set_rate(core_clk, %dKHz):%d\n",
					(ctrl->rsrcs.clk_freq_in / 1000), ret);
		goto err_set_rate;
	}

	ctrl->rsrcs.iface_clk = clk_get(ctrl->dev, "iface_clk");
	if (IS_ERR(ctrl->rsrcs.iface_clk)) {
		ret = PTR_ERR(ctrl->rsrcs.iface_clk);
		dev_err(ctrl->dev, "error on clk_get(iface_clk):%d\n", ret);
		goto err_set_rate;
	}

	return 0;

err_set_rate:
		clk_put(ctrl->rsrcs.core_clk);
		ctrl->rsrcs.core_clk = NULL;
	return ret;
}

static void i2c_msm_rsrcs_clk_teardown(struct i2c_msm_ctrl *ctrl)
{
	clk_put(ctrl->rsrcs.core_clk);
	clk_put(ctrl->rsrcs.iface_clk);
	i2c_msm_clk_path_teardown(ctrl);
}



static void i2c_msm_pm_suspend(struct device *dev)
{
	struct i2c_msm_ctrl *ctrl = dev_get_drvdata(dev);

	if (ctrl->pwr_state == I2C_MSM_PM_RT_SUSPENDED) {
		dev_err(ctrl->dev, "attempt to suspend when suspended\n");
		return;
	}
	i2c_msm_dbg(ctrl, MSM_DBG, "suspending...");
	i2c_msm_pm_pinctrl_state(ctrl, false);
	i2c_msm_clk_path_unvote(ctrl);

	if (ctrl->pwr_state != I2C_MSM_PM_SYS_SUSPENDED)
		ctrl->pwr_state = I2C_MSM_PM_RT_SUSPENDED;
	return;
}

static int i2c_msm_pm_resume(struct device *dev)
{
	struct i2c_msm_ctrl *ctrl = dev_get_drvdata(dev);

	if (ctrl->pwr_state == I2C_MSM_PM_RT_ACTIVE)
		return 0;

	i2c_msm_dbg(ctrl, MSM_DBG, "resuming...");

	i2c_msm_clk_path_vote(ctrl);
	i2c_msm_pm_pinctrl_state(ctrl, true);
	ctrl->pwr_state = I2C_MSM_PM_RT_ACTIVE;
	return 0;
}

#ifdef CONFIG_PM
static int i2c_msm_pm_sys_suspend_noirq(struct device *dev)
{
	int ret = 0;
	struct i2c_msm_ctrl *ctrl = dev_get_drvdata(dev);
	enum i2c_msm_power_state prev_state = ctrl->pwr_state;
	i2c_msm_dbg(ctrl, MSM_DBG, "pm_sys_noirq: suspending...");

	
	mutex_lock(&ctrl->xfer.mtx);
	ctrl->pwr_state = I2C_MSM_PM_SYS_SUSPENDED;
	mutex_unlock(&ctrl->xfer.mtx);
	i2c_msm_dbg(ctrl, MSM_DBG, "pm_sys_noirq: suspending...");

	if (prev_state == I2C_MSM_PM_RT_ACTIVE) {
		i2c_msm_pm_suspend(dev);
		pm_runtime_disable(dev);
		pm_runtime_set_suspended(dev);
		pm_runtime_enable(dev);
	}

	return ret;
}

static int i2c_msm_pm_sys_resume_noirq(struct device *dev)
{
	struct i2c_msm_ctrl *ctrl = dev_get_drvdata(dev);
	i2c_msm_dbg(ctrl, MSM_DBG, "pm_sys_noirq: resuming...");
	mutex_lock(&ctrl->xfer.mtx);
	ctrl->pwr_state = I2C_MSM_PM_RT_SUSPENDED;
	mutex_unlock(&ctrl->xfer.mtx);
	return  0;
}
#endif

#ifdef CONFIG_PM_RUNTIME
static void i2c_msm_pm_rt_init(struct device *dev)
{
	pm_runtime_set_suspended(dev);
	pm_runtime_set_autosuspend_delay(dev, (MSEC_PER_SEC >> 2));
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
}

static int i2c_msm_pm_rt_suspend(struct device *dev)
{
	struct i2c_msm_ctrl *ctrl = dev_get_drvdata(dev);

	i2c_msm_dbg(ctrl, MSM_DBG, "pm_runtime: suspending...");
	i2c_msm_pm_suspend(dev);
	return 0;
}

static int i2c_msm_pm_rt_resume(struct device *dev)
{
	struct i2c_msm_ctrl *ctrl = dev_get_drvdata(dev);

	i2c_msm_dbg(ctrl, MSM_DBG, "pm_runtime: resuming...");
	return  i2c_msm_pm_resume(dev);
}

#else
static void i2c_msm_pm_rt_init(struct device *dev) {}
#define i2c_msm_pm_rt_suspend NULL
#define i2c_msm_pm_rt_resume NULL
#endif

static const struct dev_pm_ops i2c_msm_pm_ops = {
#ifdef CONFIG_PM_SLEEP
	.suspend_noirq		= i2c_msm_pm_sys_suspend_noirq,
	.resume_noirq		= i2c_msm_pm_sys_resume_noirq,
#endif
	SET_RUNTIME_PM_OPS(i2c_msm_pm_rt_suspend,
			   i2c_msm_pm_rt_resume,
			   NULL)
};

static u32 i2c_msm_frmwrk_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | (I2C_FUNC_SMBUS_EMUL & ~I2C_FUNC_SMBUS_QUICK);
}

static const struct i2c_algorithm i2c_msm_frmwrk_algrtm = {
	.master_xfer	= i2c_msm_frmwrk_xfer,
	.functionality	= i2c_msm_frmwrk_func,
};

static const char const *i2c_msm_adapter_name = "MSM-I2C-v2-adapter";

static int i2c_msm_frmwrk_reg(struct platform_device *pdev,
						struct i2c_msm_ctrl *ctrl)
{
	int ret;

	i2c_set_adapdata(&ctrl->adapter, ctrl);
	ctrl->adapter.algo = &i2c_msm_frmwrk_algrtm;
	strlcpy(ctrl->adapter.name, i2c_msm_adapter_name,
						sizeof(ctrl->adapter.name));

	ctrl->adapter.nr = pdev->id;
	ctrl->adapter.dev.parent = &pdev->dev;
	ctrl->adapter.dev.of_node = pdev->dev.of_node;
	ret = i2c_add_numbered_adapter(&ctrl->adapter);
	if (ret) {
		dev_err(ctrl->dev, "error i2c_add_adapter failed\n");
		return ret;
	}

	return ret;
}

static void i2c_msm_frmwrk_unreg(struct i2c_msm_ctrl *ctrl)
{
	i2c_del_adapter(&ctrl->adapter);
}

static int i2c_msm_probe(struct platform_device *pdev)
{
	struct i2c_msm_ctrl *ctrl;
	int ret = 0;

	dev_info(&pdev->dev, "probing driver i2c-msm-v2\n");

	ctrl = devm_kzalloc(&pdev->dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;
	ctrl->dev = &pdev->dev;
	platform_set_drvdata(pdev, ctrl);
	ctrl->dbgfs.dbg_lvl         = DEFAULT_DBG_LVL;
	ctrl->dbgfs.force_xfer_mode = I2C_MSM_XFER_MODE_NONE;
	mutex_init(&ctrl->xfer.mtx);
	ctrl->pwr_state = I2C_MSM_PM_RT_SUSPENDED;

	if (!pdev->dev.of_node) {
		dev_err(&pdev->dev, "error: null device-tree node");
		return -EBADE;
	}

	ret = i2c_msm_rsrcs_process_dt(ctrl, pdev);
	if (ret) {
		dev_err(ctrl->dev, "error in process device tree node");
		return ret;
	}

	ret = i2c_msm_rsrcs_mem_init(pdev, ctrl);
	if (ret)
		goto mem_err;

	ret = i2c_msm_rsrcs_clk_init(ctrl);
	if (ret)
		goto clk_err;

	
	i2c_msm_clk_path_vote(ctrl);

	ret = i2c_msm_pm_clk_prepare_enable(ctrl);
	if (ret) {
		dev_err(ctrl->dev, "error in enabling clocks:%d\n", ret);
		goto clk_err;
	}

	ret = i2c_msm_qup_sw_reset(ctrl);
	if (ret)
		dev_err(ctrl->dev, "error error on qup software reset\n");

	i2c_msm_pm_clk_disable_unprepare(ctrl);
	i2c_msm_clk_path_unvote(ctrl);

	ret = i2c_msm_rsrcs_gpio_pinctrl_init(ctrl);
	if (ret)
		goto err_no_pinctrl;

	i2c_msm_pm_rt_init(ctrl->dev);

	ret = i2c_msm_rsrcs_irq_init(pdev, ctrl);
	if (ret)
		goto irq_err;

	i2c_msm_dbgfs_init(ctrl);

	ret = i2c_msm_frmwrk_reg(pdev, ctrl);
	if (ret)
		goto reg_err;

	i2c_msm_dbg(ctrl, MSM_PROF, "probe() completed with success");
	return 0;

reg_err:
	i2c_msm_dbgfs_teardown(ctrl);
	i2c_msm_rsrcs_irq_teardown(ctrl);
irq_err:
	i2x_msm_blk_free_cache(ctrl);
err_no_pinctrl:
	i2c_msm_rsrcs_clk_teardown(ctrl);
clk_err:
	i2c_msm_rsrcs_mem_teardown(ctrl);
mem_err:
	dev_err(ctrl->dev, "error probe() failed with err:%d\n", ret);
	devm_kfree(&pdev->dev, ctrl);
	return ret;
}

static int i2c_msm_remove(struct platform_device *pdev)
{
	struct i2c_msm_ctrl *ctrl = platform_get_drvdata(pdev);

	
	mutex_lock(&ctrl->xfer.mtx);
	ctrl->pwr_state = I2C_MSM_PM_SYS_SUSPENDED;
	pm_runtime_disable(ctrl->dev);
	
	i2c_msm_frmwrk_unreg(ctrl);
	mutex_unlock(&ctrl->xfer.mtx);
	mutex_destroy(&ctrl->xfer.mtx);

	i2c_msm_dma_teardown(ctrl);
	i2c_msm_dbgfs_teardown(ctrl);
	i2c_msm_rsrcs_irq_teardown(ctrl);
	i2c_msm_rsrcs_clk_teardown(ctrl);
	i2c_msm_rsrcs_mem_teardown(ctrl);
	i2x_msm_blk_free_cache(ctrl);
	return 0;
}

static struct of_device_id i2c_msm_dt_match[] = {
	{
		.compatible = "qcom,i2c-msm-v2",
	},
	{}
};

static struct platform_driver i2c_msm_driver = {
	.probe  = i2c_msm_probe,
	.remove = i2c_msm_remove,
	.driver = {
		.name           = "i2c-msm-v2",
		.owner          = THIS_MODULE,
		.pm             = &i2c_msm_pm_ops,
		.of_match_table = i2c_msm_dt_match,
	},
};

static int i2c_msm_init(void)
{
	return platform_driver_register(&i2c_msm_driver);
}
arch_initcall(i2c_msm_init);

static void i2c_msm_exit(void)
{
	platform_driver_unregister(&i2c_msm_driver);
}
module_exit(i2c_msm_exit);

MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:i2c-msm-v2");