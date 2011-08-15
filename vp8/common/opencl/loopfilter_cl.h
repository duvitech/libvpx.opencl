/*
 *  Copyright (c) 2011 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */


#ifndef loopfilter_cl_h
#define loopfilter_cl_h

#include "../../../vpx_ports/mem.h"

#include "../onyxc_int.h"
#include "blockd_cl.h"
#include "../loopfilter.h"

#define prototype_loopfilter_cl(sym) \
    void sym(MACROBLOCKD*, cl_mem src_base, int src_offset,  \
             int pitch, cl_mem lfi_mem, int filter_level, cl_bool use_mbflim, int count, int block_cnt)

#define prototype_loopfilter_block_cl(sym) \
    void sym(MACROBLOCKD*, unsigned char *y, unsigned char *u, unsigned char *v,\
             int ystride, int uv_stride, loop_filter_info *lfi, int filter_level)

extern void vp8_loop_filter_frame_cl
(
    VP8_COMMON *cm,
    MACROBLOCKD *mbd,
    int default_filt_lvl
);

extern prototype_loopfilter_block_cl(vp8_lf_normal_mb_v_cl);
extern prototype_loopfilter_block_cl(vp8_lf_normal_b_v_cl);
extern prototype_loopfilter_block_cl(vp8_lf_normal_mb_h_cl);
extern prototype_loopfilter_block_cl(vp8_lf_normal_b_h_cl);
extern prototype_loopfilter_block_cl(vp8_lf_simple_mb_v_cl);
extern prototype_loopfilter_block_cl(vp8_lf_simple_b_v_cl);
extern prototype_loopfilter_block_cl(vp8_lf_simple_mb_h_cl);
extern prototype_loopfilter_block_cl(vp8_lf_simple_b_h_cl);

typedef prototype_loopfilter_block_cl((*vp8_lf_block_cl_fn_t));

#endif
