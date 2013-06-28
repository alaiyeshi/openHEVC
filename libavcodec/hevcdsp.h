/*
 * HEVC video Decoder
 *
 * Copyright (C) 2012 Guillaume Martres
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_HEVCDSP_H
#define AVCODEC_HEVCDSP_H

typedef struct HEVCDSPContext {
    void (*put_pcm)(uint8_t *_dst, ptrdiff_t _stride, int size,
                    GetBitContext *gb, int pcm_bit_depth);

    void (*dequant[4])(int16_t *coeffs, int qp);

    void (*transquant_bypass[4])(uint8_t *_dst, int16_t *coeffs, ptrdiff_t _stride);

    void (*transform_skip)(uint8_t *dst, int16_t *coeffs, ptrdiff_t stride);

    void (*transform_4x4_luma_add)(uint8_t *dst, int16_t *coeffs, ptrdiff_t stride);

    void (*transform_add[4])(uint8_t *dst, int16_t *coeffs, ptrdiff_t _stride);

    void (*sao_band_filter)(uint8_t *dst, uint8_t *src, ptrdiff_t stride, int *sao_offset_val,
                            int sao_left_class, int width, int height);

    void (*sao_edge_filter)(uint8_t *dst, uint8_t *src, ptrdiff_t stride, int *sao_offset_val,
                            int sao_eo_class, int at_top_border, int at_bottom_border,
                            int at_left_border, int at_right_border,
                            int width, int height);
    void (*sao_band_filter_wpp[4])( uint8_t *_dst, uint8_t *_src, ptrdiff_t _stride, struct SAOParams *sao, int *borders, int width, int height, int c_idx);

    void (*sao_edge_filter_wpp[4])(uint8_t *_dst, uint8_t *_src, ptrdiff_t _stride,  struct SAOParams *sao, int *borders, int _width, int _height, int c_idx);


    void (*put_hevc_qpel[4][4])(int16_t *dst, ptrdiff_t dststride, uint8_t *src, ptrdiff_t srcstride,
                                int width, int height, int16_t* mcbuffer);

    void (*put_hevc_epel[2][2])(int16_t *dst, ptrdiff_t dststride, uint8_t *src, ptrdiff_t srcstride,
                             int width, int height, int mx, int my, int16_t* mcbuffer);

    void (*put_unweighted_pred)(uint8_t *dst, ptrdiff_t dststride, int16_t *src, ptrdiff_t srcstride,
                                int width, int height);
    void (*put_weighted_pred_avg)(uint8_t *dst, ptrdiff_t dststride, int16_t *src1, int16_t *src2,
                                  ptrdiff_t srcstride, int width, int height);

    void (*weighted_pred)(uint8_t denom, int16_t wlxFlag, int16_t olxFlag, uint8_t *dst, ptrdiff_t dststride, int16_t *src,
                                  ptrdiff_t srcstride, int width, int height);
    void (*weighted_pred_avg)(uint8_t denom, int16_t wl0Flag, int16_t wl1Flag, int16_t ol0Flag, int16_t ol1Flag,
                                   uint8_t *dst, ptrdiff_t dststride, int16_t *src1, int16_t *src2,
                                   ptrdiff_t srcstride, int width, int height);
    void (*hevc_loop_filter_luma[2])(uint8_t *_pix, ptrdiff_t _xstride, ptrdiff_t _ystride, int no_p, int no_q, int _beta, int _tc);
    void (*hevc_loop_filter_chroma)(uint8_t *_pix, ptrdiff_t _xstride, ptrdiff_t _ystride, int no_p, int no_q, int _tc);
    void (*upsample_base_layer_frame)(AVFrame *FrameEL, AVFrame *FrameBL, AVFrame *Buffer, const int32_t enabled_up_sample_filter_luma[16][8], const int32_t enabled_up_sample_filter_chroma[12][4], struct HEVCWindow *Enhscal);
} HEVCDSPContext;

void ff_hevc_dsp_init(HEVCDSPContext *hpc, int bit_depth);

#endif /* AVCODEC_HEVCDSP_H */
