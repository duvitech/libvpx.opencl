/*
 *  Copyright (c) 2011 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

//for the decoder, all subpixel prediction is done in this file.
//
//Need to determine some sort of mechanism for easily determining SIXTAP/BILINEAR
//and what arguments to feed into the kernels. These kernels SHOULD be 2-pass,
//and ideally there'd be a data structure that determined what static arguments
//to pass in.
//
//Also, the only external functions being called here are the subpixel prediction
//functions. Hopefully this means no worrying about when to copy data back/forth.

#include "../../../vpx_ports/config.h"
//#include "../recon.h"
#include "../subpixel.h"
//#include "../blockd.h"
//#include "../reconinter.h"
#if CONFIG_RUNTIME_CPU_DETECT
//#include "../onyxc_int.h"
#endif

#include "vp8_opencl.h"
#include "filter_cl.h"
#include "reconinter_cl.h"
#include "blockd_cl.h"

#include <stdio.h>

/* use this define on systems where unaligned int reads and writes are
 * not allowed, i.e. ARM architectures
 */
/*#define MUST_BE_ALIGNED*/


static const int bbb[4] = {0, 2, 8, 10};

static void vp8_memcpy(
    unsigned char *src_base,
    int src_offset,
    int src_stride,
    unsigned char *dst_base,
    int dst_offset,
    int dst_stride,
    int num_bytes,
    int num_iter
){

    int i,r;
    unsigned char *src = &src_base[src_offset];
    unsigned char *dst = &dst_base[dst_offset];
    src_offset = dst_offset = 0;

    for (r = 0; r < num_iter; r++){
        for (i = 0; i < num_bytes; i++){
            src_offset = r*src_stride + i;
            dst_offset = r*dst_stride + i;
            dst[dst_offset] = src[src_offset];
        }
    }
}

static void vp8_copy_mem_cl(
    cl_command_queue cq,
    unsigned char *src_base,
    cl_mem src_mem,
    int src_offset,
    int src_stride,
    unsigned char *dst_base,
    cl_mem dst_mem,
    int dst_offset,
    int dst_stride,
    int num_bytes,
    int num_iter
){

    unsigned char *src = src_base + src_offset;
    unsigned char *dst = dst_base + dst_offset;

    int free_src = 0, free_dst = 0;

    int err;
    size_t src_len = (num_iter - 1)*src_stride + num_bytes;
    size_t dst_len = (num_iter - 1)*dst_stride + num_bytes;
    size_t global[2] = {num_bytes, num_iter};

    if (src_mem == NULL){
        src_offset = 0;
        CL_CREATE_BUF( cq, src_mem, CL_MEM_WRITE_ONLY,
            sizeof (unsigned char) * src_len, src,
        );
        free_src = 1;
    }

    if (dst_mem == NULL){
        dst_offset = 0;
        CL_CREATE_BUF( cq, dst_mem, CL_MEM_WRITE_ONLY,
            sizeof (unsigned char) * dst_len, dst,
        );
        free_dst = 1;
    }


    /* Set kernel arguments */
    err  = clSetKernelArg(cl_data.vp8_memcpy_kernel, 0, sizeof (cl_mem), &src_mem);
    err |= clSetKernelArg(cl_data.vp8_memcpy_kernel, 1, sizeof (int), &src_offset);
    err |= clSetKernelArg(cl_data.vp8_memcpy_kernel, 2, sizeof (int), &src_stride);
    err |= clSetKernelArg(cl_data.vp8_memcpy_kernel, 3, sizeof (cl_mem), &dst_mem);
    err |= clSetKernelArg(cl_data.vp8_memcpy_kernel, 4, sizeof (int), &dst_offset);
    err |= clSetKernelArg(cl_data.vp8_memcpy_kernel, 5, sizeof (int), &dst_stride);
    err |= clSetKernelArg(cl_data.vp8_memcpy_kernel, 6, sizeof (int), &num_bytes);
    err |= clSetKernelArg(cl_data.vp8_memcpy_kernel, 7, sizeof (int), &num_iter);
    CL_CHECK_SUCCESS( cq, err != CL_SUCCESS,
        "Error: Failed to set kernel arguments!\n",
        return,
    );

    /* Execute the kernel */
    err = clEnqueueNDRangeKernel( cq, cl_data.vp8_memcpy_kernel, 2, NULL, global, NULL , 0, NULL, NULL);
    CL_CHECK_SUCCESS( cq, err != CL_SUCCESS,
        "Error: Failed to execute kernel!\n",
        return,
    );

    if (free_src == 1){
        clReleaseMemObject(src_mem);
    }

    if (free_dst == 1){
        /* Read back the result data from the device */
        err = clEnqueueReadBuffer(cq, dst_mem, CL_FALSE, 0,
                sizeof (unsigned char) * dst_len, dst, 0, NULL, NULL);
        CL_CHECK_SUCCESS( cq, err != CL_SUCCESS,
            "Error: Failed to read output array!\n",
            return,
        );

        clReleaseMemObject(dst_mem);
    }
}

static void vp8_build_inter_predictors_b_cl(MACROBLOCKD *x, BLOCKD *d, int pitch)
{
    int r;
    unsigned char *ptr_base = *(d->base_pre);
    int ptr_offset = d->pre + (d->bmi.mv.as_mv.row >> 3) * d->pre_stride + (d->bmi.mv.as_mv.col >> 3);

    vp8_subpix_cl_fn_t sppf;

    int pre_dist = *d->base_pre - x->pre.buffer_alloc;
    cl_mem pre_mem = x->pre.buffer_mem;

    if (d->sixtap_filter == CL_TRUE)
        sppf = vp8_sixtap_predict4x4_cl;
    else
        sppf = vp8_bilinear_predict4x4_cl;

    //ptr_base a.k.a. d->base_pre is the start of the
    //Macroblock's y_buffer, u_buffer, or v_buffer

    if (d->bmi.mv.as_mv.row & 7 || d->bmi.mv.as_mv.col & 7)
    {
        sppf(d->cl_commands, ptr_base, pre_mem, pre_dist+ptr_offset, d->pre_stride, d->bmi.mv.as_mv.col & 7, d->bmi.mv.as_mv.row & 7, d->predictor_base, d->cl_predictor_mem, d->predictor_offset, pitch);
    }
    else
    {
        vp8_copy_mem_cl(d->cl_commands, ptr_base, pre_mem, pre_dist+ptr_offset,d->pre_stride,NULL,d->cl_predictor_mem, d->predictor_offset,pitch,4,4);
    }
}


static void vp8_build_inter_predictors4b_cl(MACROBLOCKD *x, BLOCKD *d, int pitch)
{
    unsigned char *ptr_base = *(d->base_pre);
    int ptr_offset = d->pre + (d->bmi.mv.as_mv.row >> 3) * d->pre_stride + (d->bmi.mv.as_mv.col >> 3);

    int pre_dist = *d->base_pre - x->pre.buffer_alloc;
    cl_mem pre_mem = x->pre.buffer_mem;

    if (d->bmi.mv.as_mv.row & 7 || d->bmi.mv.as_mv.col & 7)
    {
            if (d->sixtap_filter == CL_TRUE)
                vp8_sixtap_predict8x8_cl(d->cl_commands, ptr_base, pre_mem, pre_dist+ptr_offset, d->pre_stride, d->bmi.mv.as_mv.col & 7, d->bmi.mv.as_mv.row & 7, d->predictor_base, d->cl_predictor_mem, d->predictor_offset, pitch);
            else
                vp8_bilinear_predict8x8_cl(d->cl_commands, ptr_base, pre_mem, pre_dist+ptr_offset, d->pre_stride, d->bmi.mv.as_mv.col & 7, d->bmi.mv.as_mv.row & 7, d->predictor_base, d->cl_predictor_mem, d->predictor_offset, pitch);
    }
    else
    {
        vp8_copy_mem_cl(d->cl_commands, ptr_base, pre_mem, pre_dist+ptr_offset, d->pre_stride, NULL, d->cl_predictor_mem, d->predictor_offset, pitch, 8, 8);
    }


}

static void vp8_build_inter_predictors2b_cl(MACROBLOCKD *x, BLOCKD *d, int pitch)
{
    unsigned char *ptr_base = *(d->base_pre);

    int ptr_offset = d->pre + (d->bmi.mv.as_mv.row >> 3) * d->pre_stride + (d->bmi.mv.as_mv.col >> 3);

    int pre_dist = *d->base_pre - x->pre.buffer_alloc;
    cl_mem pre_mem = x->pre.buffer_mem;

    if (d->bmi.mv.as_mv.row & 7 || d->bmi.mv.as_mv.col & 7)
    {
        if (d->sixtap_filter == CL_TRUE)
            vp8_sixtap_predict8x4_cl(d->cl_commands,ptr_base,pre_mem,pre_dist+ptr_offset, d->pre_stride, d->bmi.mv.as_mv.col & 7, d->bmi.mv.as_mv.row & 7, d->predictor_base, d->cl_predictor_mem, d->predictor_offset, pitch);
        else
            vp8_bilinear_predict8x4_cl(d->cl_commands,ptr_base,pre_mem,pre_dist+ptr_offset, d->pre_stride, d->bmi.mv.as_mv.col & 7, d->bmi.mv.as_mv.row & 7, d->predictor_base, d->cl_predictor_mem, d->predictor_offset, pitch);
    }
    else
    {
        vp8_copy_mem_cl(d->cl_commands, ptr_base, pre_mem, pre_dist+ptr_offset, d->pre_stride, NULL, d->cl_predictor_mem, d->predictor_offset, pitch, 8, 4);
    }
}


void vp8_build_inter_predictors_mbuv_cl(MACROBLOCKD *x)
{
    int i;

    vp8_cl_mb_prep(x, PREDICTOR|PRE_BUF);

    if (x->mode_info_context->mbmi.ref_frame != INTRA_FRAME &&
        x->mode_info_context->mbmi.mode != SPLITMV)
    {

        unsigned char *pred_base = x->predictor;
        int upred_offset = 256;
        int vpred_offset = 320;

        int mv_row = x->block[16].bmi.mv.as_mv.row;
        int mv_col = x->block[16].bmi.mv.as_mv.col;
        int offset;

        unsigned char *pre_base = x->pre.buffer_alloc;
        cl_mem pre_mem = x->pre.buffer_mem;
        int upre_off = x->pre.u_buffer - pre_base;
        int vpre_off = x->pre.v_buffer - pre_base;
        int pre_stride = x->block[16].pre_stride;

        offset = (mv_row >> 3) * pre_stride + (mv_col >> 3);

        if ((mv_row | mv_col) & 7)
        {
            if (cl_initialized == CL_SUCCESS && x->sixtap_filter == CL_TRUE){
                vp8_sixtap_predict8x8_cl(x->block[16].cl_commands,pre_base, pre_mem, upre_off+offset, pre_stride, mv_col & 7, mv_row & 7, pred_base, x->cl_predictor_mem, upred_offset, 8);
                vp8_sixtap_predict8x8_cl(x->block[16].cl_commands,pre_base, pre_mem, vpre_off+offset, pre_stride, mv_col & 7, mv_row & 7, pred_base, x->cl_predictor_mem, vpred_offset, 8);
            }
            else{
                vp8_bilinear_predict8x8_cl(x->block[16].cl_commands,pre_base, pre_mem, upre_off+offset, pre_stride, mv_col & 7, mv_row & 7, pred_base, x->cl_predictor_mem, upred_offset, 8);
                vp8_bilinear_predict8x8_cl(x->block[16].cl_commands,pre_base, pre_mem, vpre_off+offset, pre_stride, mv_col & 7, mv_row & 7, pred_base, x->cl_predictor_mem, vpred_offset, 8);
            }
        }
        else
        {
            vp8_copy_mem_cl(x->block[16].cl_commands, pre_base, pre_mem, upre_off+offset, pre_stride, NULL, x->cl_predictor_mem, upred_offset, 8, 8, 8);
            vp8_copy_mem_cl(x->block[16].cl_commands, pre_base, pre_mem, vpre_off+offset, pre_stride, NULL, x->cl_predictor_mem, vpred_offset, 8, 8, 8);
        }
    }
    else
    {
        // Can probably batch these operations as well, but not tested in decoder
        // (or at least the test videos I've been using.
        for (i = 16; i < 24; i += 2)
        {
            BLOCKD *d0 = &x->block[i];
            BLOCKD *d1 = &x->block[i+1];
            if (d0->bmi.mv.as_int == d1->bmi.mv.as_int)
                vp8_build_inter_predictors2b_cl(x, d0, 8);
            else
            {
                vp8_build_inter_predictors_b_cl(x, d0, 8);
                vp8_build_inter_predictors_b_cl(x, d1, 8);
            }
        }
    }

    vp8_cl_mb_finish(x, PREDICTOR);
}

void vp8_build_inter_predictors_mb_cl(MACROBLOCKD *x)
{
    //If CL is running in encoder, need to call following before proceeding.
    //vp8_cl_mb_prep(x, PRE_BUF);

    if (x->mode_info_context->mbmi.ref_frame != INTRA_FRAME &&
        x->mode_info_context->mbmi.mode != SPLITMV)
    {
        int offset;
        unsigned char *pred_base = x->predictor;
        int upred_offset = 256;
        int vpred_offset = 320;

        int mv_row = x->mode_info_context->mbmi.mv.as_mv.row;
        int mv_col = x->mode_info_context->mbmi.mv.as_mv.col;
        int pre_stride = x->block[0].pre_stride;

        unsigned char *pre_base = x->pre.buffer_alloc;
        cl_mem pre_mem = x->pre.buffer_mem;
        int ypre_off = x->pre.y_buffer - pre_base + (mv_row >> 3) * pre_stride + (mv_col >> 3);
        int upre_off = x->pre.u_buffer - pre_base;
        int vpre_off = x->pre.v_buffer - pre_base;

        if ((mv_row | mv_col) & 7)
        {
            if (cl_initialized == CL_SUCCESS && x->sixtap_filter == CL_TRUE){
                vp8_sixtap_predict16x16_cl(x->cl_commands, pre_base, pre_mem, ypre_off, pre_stride, mv_col & 7, mv_row & 7, pred_base, x->cl_predictor_mem, 0, 16);
            }
            else
                vp8_bilinear_predict16x16_cl(x->cl_commands, pre_base, pre_mem,  ypre_off, pre_stride, mv_col & 7, mv_row & 7, pred_base, x->cl_predictor_mem, 0, 16);
        }
        else
        {
            //16x16 copy
            vp8_copy_mem_cl(x->cl_commands, pre_base, pre_mem, ypre_off, pre_stride, pred_base, x->cl_predictor_mem, 0, 16, 16, 16);
        }


        mv_row = x->block[16].bmi.mv.as_mv.row;
        mv_col = x->block[16].bmi.mv.as_mv.col;
        pre_stride >>= 1;
        offset = (mv_row >> 3) * pre_stride + (mv_col >> 3);

        if ((mv_row | mv_col) & 7)
        {
            if (x->sixtap_filter == CL_TRUE){
                vp8_sixtap_predict8x8_cl(x->cl_commands, pre_base, pre_mem, upre_off+offset, pre_stride, mv_col & 7, mv_row & 7, pred_base, x->cl_predictor_mem, upred_offset, 8);
                vp8_sixtap_predict8x8_cl(x->cl_commands, pre_base, pre_mem, vpre_off+offset, pre_stride, mv_col & 7, mv_row & 7, pred_base, x->cl_predictor_mem, vpred_offset, 8);
            }
            else {
                vp8_bilinear_predict8x8_cl(x->cl_commands, pre_base, pre_mem, upre_off+offset, pre_stride, mv_col & 7, mv_row & 7, pred_base, x->cl_predictor_mem, upred_offset, 8);
                vp8_bilinear_predict8x8_cl(x->cl_commands, pre_base, pre_mem, vpre_off+offset, pre_stride, mv_col & 7, mv_row & 7, pred_base, x->cl_predictor_mem, vpred_offset, 8);
            }
        }
        else
        {
            vp8_copy_mem_cl(x->cl_commands, pre_base, pre_mem, upre_off+offset, pre_stride, pred_base, x->cl_predictor_mem, upred_offset, 8, 8, 8);
            vp8_copy_mem_cl(x->cl_commands, pre_base, pre_mem, vpre_off+offset, pre_stride, pred_base, x->cl_predictor_mem, vpred_offset, 8, 8, 8);
        }
    }
    else
    {
        int i;

        if (x->mode_info_context->mbmi.partitioning < 3)
        {
            for (i = 0; i < 4; i++)
            {
                BLOCKD *d = &x->block[bbb[i]];
                vp8_build_inter_predictors4b_cl(x, d, 16);
            }
        }
        else
        {
            /* This loop can be done in any order... No dependencies.*/
            /* Also, d0/d1 can be decoded simultaneously */
            for (i = 0; i < 16; i += 2)
            {
                BLOCKD *d0 = &x->block[i];
                BLOCKD *d1 = &x->block[i+1];

                if (d0->bmi.mv.as_int == d1->bmi.mv.as_int)
                    vp8_build_inter_predictors2b_cl(x, d0, 16);
                else
                {
                    vp8_build_inter_predictors_b_cl(x, d0, 16);
                    vp8_build_inter_predictors_b_cl(x, d1, 16);
                }
            }
        }

        /* Another case of re-orderable/batchable loop */
        for (i = 16; i < 24; i += 2)
        {
            BLOCKD *d0 = &x->block[i];
            BLOCKD *d1 = &x->block[i+1];

            if (d0->bmi.mv.as_int == d1->bmi.mv.as_int)
                vp8_build_inter_predictors2b_cl(x, d0, 8);
            else
            {
                vp8_build_inter_predictors_b_cl(x, d0, 8);
                vp8_build_inter_predictors_b_cl(x, d1, 8);
            }
        }
    }

    vp8_cl_mb_finish(x, PREDICTOR);
}


/* The following functions are written for skip_recon_mb() to call. Since there is no recon in this
 * situation, we can write the result directly to dst buffer instead of writing it to predictor
 * buffer and then copying it to dst buffer.
 */
static void vp8_build_inter_predictors_b_s_cl(BLOCKD *d, unsigned char *dst_base, int dst_offset)
{
    int r;
    unsigned char *ptr_base = *(d->base_pre);
    int dst_stride = d->dst_stride;
    int pre_stride = d->pre_stride;
    int ptr_offset = d->pre + (d->bmi.mv.as_mv.row >> 3) * d->pre_stride + (d->bmi.mv.as_mv.col >> 3);
    vp8_subpix_cl_fn_t sppf;

    if (d->sixtap_filter == CL_TRUE){
        sppf = vp8_sixtap_predict4x4_cl;
    } else
        sppf = vp8_bilinear_predict4x4_cl;
        
    if (d->bmi.mv.as_mv.row & 7 || d->bmi.mv.as_mv.col & 7)
    {
        sppf(d->cl_commands, ptr_base, NULL, ptr_offset, pre_stride, d->bmi.mv.as_mv.col & 7, d->bmi.mv.as_mv.row & 7, dst_base, NULL, dst_offset, dst_stride);
    }
    else
    {
        vp8_copy_mem_cl(d->cl_commands, ptr_base, NULL,ptr_offset,pre_stride,dst_base, NULL,dst_offset,dst_stride,4,4);
    }
}


/* Never actually used... anywhere */
void vp8_build_inter_predictors_mb_s_cl(MACROBLOCKD *x)
{
    unsigned char *pred_ptr = x->predictor;

    unsigned char *dst_base = x->dst.buffer_alloc;
    int ydst_off = x->dst.y_buffer - dst_base;
    int udst_off = x->dst.u_buffer - dst_base;
    int vdst_off = x->dst.v_buffer - dst_base;

    if (x->mode_info_context->mbmi.mode != SPLITMV)
    {
        int offset;
        unsigned char *pre_base = x->pre.buffer_alloc;
        int ypre_off = x->pre.y_buffer - pre_base;
        int upre_off = x->pre.u_buffer - pre_base;
        int vpre_off = x->pre.v_buffer - pre_base;

        int mv_row = x->mode_info_context->mbmi.mv.as_mv.row;
        int mv_col = x->mode_info_context->mbmi.mv.as_mv.col;
        int pre_stride = x->dst.y_stride;

        int ptr_offset = (mv_row >> 3) * pre_stride + (mv_col >> 3);

        

        if ((mv_row | mv_col) & 7)
        {
            if (x->sixtap_filter == CL_TRUE)
                vp8_sixtap_predict16x16_cl(x->cl_commands, pre_base, NULL, ypre_off+ptr_offset, pre_stride, mv_col & 7, mv_row & 7, dst_base, NULL, ydst_off, x->dst.y_stride);
            else
                vp8_bilinear_predict16x16_cl(x->cl_commands, pre_base, NULL, ypre_off+ptr_offset, pre_stride, mv_col & 7, mv_row & 7, dst_base, NULL, ydst_off, x->dst.y_stride);
        }
        else
        {
            CL_FINISH(x->cl_commands);
            vp8_memcpy(pre_base, ypre_off+ptr_offset, pre_stride, dst_base, ydst_off, x->dst.y_stride, 16, 16);
            //vp8_copy_mem_cl(x->cl_commands, pre_base, NULL, ypre_off+ptr_offset, pre_stride, dst_base, NULL, ydst_off, x->dst.y_stride, 16, 16);
        }

        mv_row = x->block[16].bmi.mv.as_mv.row;
        mv_col = x->block[16].bmi.mv.as_mv.col;
        pre_stride >>= 1;
        offset = (mv_row >> 3) * pre_stride + (mv_col >> 3);

        if ((mv_row | mv_col) & 7)
        {
            if (x->sixtap_filter == CL_TRUE){
                vp8_sixtap_predict8x8_cl(x->cl_commands, pre_base, NULL, upre_off+offset, pre_stride, mv_col & 7, mv_row & 7, dst_base, NULL, udst_off, x->dst.uv_stride);
                vp8_sixtap_predict8x8_cl(x->cl_commands, pre_base, NULL, vpre_off+offset, pre_stride, mv_col & 7, mv_row & 7, dst_base, NULL, vdst_off, x->dst.uv_stride);
            } else {
                vp8_bilinear_predict8x8_cl(x->cl_commands, pre_base, NULL, upre_off+offset, pre_stride, mv_col & 7, mv_row & 7, dst_base, NULL, udst_off, x->dst.uv_stride);
                vp8_bilinear_predict8x8_cl(x->cl_commands, pre_base, NULL, vpre_off+offset, pre_stride, mv_col & 7, mv_row & 7, dst_base, NULL, vdst_off, x->dst.uv_stride);
            }
        }
        else
        {
            CL_FINISH(x->block[16].cl_commands);
            vp8_memcpy(pre_base, upre_off+offset, pre_stride, dst_base, udst_off, x->dst.uv_stride, 8, 8);
            vp8_memcpy(pre_base, vpre_off+offset, pre_stride, dst_base, vdst_off, x->dst.uv_stride, 8, 8);

            //vp8_copy_mem_cl(x->block[16].cl_commands, pre_base, NULL, upre_off+offset, pre_stride, dst_base, NULL, udst_off, x->dst.uv_stride, 8, 8);
            //vp8_copy_mem_cl(x->block[16].cl_commands, pre_base, NULL, vpre_off+offset, pre_stride, dst_base, NULL, vdst_off, x->dst.uv_stride, 8, 8);
        }
    }
    else
    {
        /* note: this whole ELSE part is not executed at all. So, no way to test the correctness of my modification. Later,
         * if sth is wrong, go back to what it is in build_inter_predictors_mb.
         *
         * ACW: note: Not sure who the above comment belongs to.
         */
        int i;

        if (x->mode_info_context->mbmi.partitioning < 3)
        {
            for (i = 0; i < 4; i++)
            {
                BLOCKD *d = &x->block[bbb[i]];
                /*vp8_build_inter_predictors4b(x, d, 16);*/

                {
                    unsigned char *ptr_base = *(d->base_pre);
                    int ptr_offset = d->pre + (d->bmi.mv.as_mv.row >> 3) * d->pre_stride + (d->bmi.mv.as_mv.col >> 3);

                    if (d->bmi.mv.as_mv.row & 7 || d->bmi.mv.as_mv.col & 7)
                    {
                        if (x->sixtap_filter == CL_TRUE)
                            vp8_sixtap_predict8x8_cl(d->cl_commands, ptr_base, NULL, ptr_offset, d->pre_stride, d->bmi.mv.as_mv.col & 7, d->bmi.mv.as_mv.row & 7, dst_base, NULL, ydst_off, x->dst.y_stride);
                        else
                            vp8_bilinear_predict8x8_cl(d->cl_commands, ptr_base, NULL, ptr_offset, d->pre_stride, d->bmi.mv.as_mv.col & 7, d->bmi.mv.as_mv.row & 7, dst_base, NULL, ydst_off, x->dst.y_stride);
                    }
                    else
                    {
                        CL_FINISH(x->cl_commands);
                        vp8_memcpy(ptr_base, ptr_offset, d->pre_stride, dst_base, ydst_off, x->dst.y_stride, 8, 8);
                        //vp8_copy_mem_cl(x->cl_commands, ptr_base, NULL, ptr_offset, d->pre_stride, dst_base, NULL, ydst_off, x->dst.y_stride, 8, 8);
                    }
                }
            }
        }
        else
        {
            for (i = 0; i < 16; i += 2)
            {
                BLOCKD *d0 = &x->block[i];
                BLOCKD *d1 = &x->block[i+1];

                if (d0->bmi.mv.as_int == d1->bmi.mv.as_int)
                {
                    /*vp8_build_inter_predictors2b(x, d0, 16);*/
                    unsigned char *ptr_base = *(d0->base_pre);
                    int ptr_offset = d0->pre + (d0->bmi.mv.as_mv.row >> 3) * d0->pre_stride + (d0->bmi.mv.as_mv.col >> 3);

                    if (d0->bmi.mv.as_mv.row & 7 || d0->bmi.mv.as_mv.col & 7)
                    {
                        if (d0->sixtap_filter == CL_TRUE)
                            vp8_sixtap_predict8x4_cl(d0->cl_commands, ptr_base, NULL, ptr_offset, d0->pre_stride, d0->bmi.mv.as_mv.col & 7, d0->bmi.mv.as_mv.row & 7, dst_base, NULL, ydst_off, x->dst.y_stride);
                        else
                            vp8_bilinear_predict8x4_cl(d0->cl_commands, ptr_base, NULL,ptr_offset, d0->pre_stride, d0->bmi.mv.as_mv.col & 7, d0->bmi.mv.as_mv.row & 7, dst_base, NULL, ydst_off, x->dst.y_stride);
                    }
                    else
                    {
                        CL_FINISH(x->cl_commands);
                        vp8_memcpy(ptr_base, ptr_offset, d0->pre_stride, dst_base, ydst_off, x->dst.y_stride, 8, 4);
                        //vp8_copy_mem_cl(x->cl_commands, ptr_base, NULL, ptr_offset, d0->pre_stride, dst_base, NULL, ydst_off, x->dst.y_stride, 8, 4);
                    }
                }
                else
                {
                    vp8_build_inter_predictors_b_s_cl(d0, dst_base, ydst_off);
                    vp8_build_inter_predictors_b_s_cl(d1, dst_base, ydst_off);
                }
            }
        }

        for (i = 16; i < 24; i += 2)
        {
            BLOCKD *d0 = &x->block[i];
            BLOCKD *d1 = &x->block[i+1];

            if (d0->bmi.mv.as_int == d1->bmi.mv.as_int)
            {
                /*vp8_build_inter_predictors2b(x, d0, 8);*/
                unsigned char *ptr_base = *(d0->base_pre);
                int ptr_offset = d0->pre + (d0->bmi.mv.as_mv.row >> 3) * d0->pre_stride + (d0->bmi.mv.as_mv.col >> 3);

                if (d0->bmi.mv.as_mv.row & 7 || d0->bmi.mv.as_mv.col & 7)
                {
                    if (d0->sixtap_filter || CL_TRUE)
                        vp8_sixtap_predict8x4_cl(d0->cl_commands, ptr_base, NULL, ptr_offset, d0->pre_stride,
                            d0->bmi.mv.as_mv.col & 7, d0->bmi.mv.as_mv.row & 7,
                            dst_base, NULL, ydst_off, x->dst.uv_stride);
                    else
                        vp8_bilinear_predict8x4_cl(d0->cl_commands, ptr_base, NULL, ptr_offset, d0->pre_stride,
                            d0->bmi.mv.as_mv.col & 7, d0->bmi.mv.as_mv.row & 7,
                            dst_base, NULL, ydst_off, x->dst.uv_stride);
                }
                else
                {
                    CL_FINISH(x->cl_commands);
                    vp8_memcpy(ptr_base, ptr_offset,
                        d0->pre_stride, dst_base, ydst_off, x->dst.uv_stride, 8, 4);
                    //vp8_copy_mem_cl(x->cl_commands, ptr_base, NULL, ptr_offset,
                    //    d0->pre_stride, dst_base, NULL, ydst_off, x->dst.uv_stride, 8, 4);
                }
            }
            else
            {
                vp8_build_inter_predictors_b_s_cl(d0, dst_base, ydst_off);
                vp8_build_inter_predictors_b_s_cl(d1, dst_base, ydst_off);
            }
        } //end for
    }
    CL_FINISH(x->cl_commands)
}
