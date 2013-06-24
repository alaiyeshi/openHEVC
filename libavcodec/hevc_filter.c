#include "libavutil/attributes.h"
#include "libavutil/common.h"
#include "libavutil/pixdesc.h"
#include "libavutil/internal.h"
#include "cabac_functions.h"
#include "golomb.h"
#include "hevcdata.h"
#include "hevc.h"
#include "libavutil/opt.h"
#include "libavutil/md5.h"
#include "bit_depth_template.c"

#define LUMA 0
#define CB 1
#define CR 2

static int chroma_tc(HEVCContext *s, int qp_y, int c_idx)
{
    static int qp_c[] = { 29, 30, 31, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37 };
    int qp_i, offset;
    int qp;
    int idxt;

    // slice qp offset is not used for deblocking
    if (c_idx == 1)
        offset = s->pps->cb_qp_offset;
    else
        offset = s->pps->cr_qp_offset;

    qp_i = av_clip_c(qp_y + offset, - s->sps->qp_bd_offset, 57);
    if (qp_i < 30)
        qp = qp_i;
    else if (qp_i > 43)
        qp = qp_i - 6;
    else
        qp = qp_c[qp_i - 30];

    qp += s->sps->qp_bd_offset;

    idxt = av_clip_c(qp + DEFAULT_INTRA_TC_OFFSET + s->sh.tc_offset, 0, 53);
    return tctable[idxt];
}
static int get_qPy_pred(HEVCContext *s, int xC, int yC, int xBase, int yBase, int entry)
{
    int Log2CtbSizeY         = s->sps->log2_ctb_size;
    int MinCuQpDeltaSizeMask     = (1 << (Log2CtbSizeY - s->pps->diff_cu_qp_delta_depth)) - 1;
    int xQg                  = xC    - ( xC    & MinCuQpDeltaSizeMask );
    int yQg                  = yC    - ( yC    & MinCuQpDeltaSizeMask );
    int xQgBase              = xBase - ( xBase & MinCuQpDeltaSizeMask );
    int yQgBase              = yBase - ( yBase & MinCuQpDeltaSizeMask );
    int log2_min_cb_size     = s->sps->log2_min_coding_block_size;
    int pic_width            = s->sps->pic_width_in_luma_samples>>log2_min_cb_size;
    int x                    = xQg >> log2_min_cb_size;
    int y                    = yQg >> log2_min_cb_size;
    int qPy_pred;
    int qPy_a;
    int qPy_b;
    int availableA           = (xQg & ((1<<Log2CtbSizeY)-1)) != 0 && xQg == xQgBase;
    int availableB           = (yQg & ((1<<Log2CtbSizeY)-1)) != 0 && yQg == yQgBase;
    // qPy_pred
    if (s->isFirstQPgroup[entry] != 0) {
        s->isFirstQPgroup[entry] = 0;
        qPy_pred = s->sh.slice_qp;
    } else
        qPy_pred = s->qp_y[entry];
    // qPy_a
    if (availableA == 0)
        qPy_a = qPy_pred;
    else
        qPy_a = s->qp_y_tab[(x-1) + y * pic_width];

    // qPy_b
    if (availableB == 0)
        qPy_b = qPy_pred;
    else
        qPy_b = s->qp_y_tab[x + (y-1) * pic_width];
    return (qPy_a + qPy_b + 1) >> 1;
}
void ff_hevc_set_qPy(HEVCContext *s, int xC, int yC, int xBase, int yBase, int entry)
{
    if (s->tu[entry].cu_qp_delta != 0)
        s->qp_y[entry] = ((get_qPy_pred(s, xC, yC, xBase, yBase, entry) + s->tu[entry].cu_qp_delta + 52 + 2 * s->sps->qp_bd_offset) %
                (52 + s->sps->qp_bd_offset)) - s->sps->qp_bd_offset;
    else
        s->qp_y[entry] = get_qPy_pred(s, xC, yC, xBase, yBase, entry);
}
static int get_qPy(HEVCContext *s, int xC, int yC)
{
    int log2_min_cb_size = s->sps->log2_min_coding_block_size;
    int pic_width        = s->sps->pic_width_in_luma_samples>>log2_min_cb_size;
    int x                = xC >> log2_min_cb_size;
    int y                = yC >> log2_min_cb_size;
    return s->qp_y_tab[x + y * pic_width];
}

static void copy_CTB(uint8_t *dst, uint8_t *src, int width, int height, int stride){
    int i;
    
    for(i=0; i< height; i++){
        memcpy(dst, src, width);
        dst += stride;
        src += stride;
    }
}

#define CTB(tab, x, y) ((tab)[(y) * s->sps->pic_width_in_ctbs + (x)])

void ff_hevc_sao_filter_CTB(HEVCContext *s, int x, int y, int c_idx_min, int c_idx_max)
{
    //  TODO: This should be easily parallelizable
    //  TODO: skip CBs when (cu_transquant_bypass_flag || (pcm_loop_filter_disable_flag && pcm_flag))
    
    int c_idx = 0;
    int class=1, class_index;
    int  edges[4]; // 0 left 1 top 2 right 3 bottom
    struct SAOParams *sao[4];
    int classes[4];
    int x_shift = 0, y_shift = 0;
    int x_ctb = x>>s->sps->log2_ctb_size;
    int y_ctb = y>>s->sps->log2_ctb_size;
    sao[0]= &CTB(s->sao, x_ctb, y_ctb);
    edges[0]   =  x_ctb == 0;
    edges[1]   =  y_ctb == 0;
    edges[2]   =  x_ctb == (s->sps->pic_width_in_ctbs - 1);
    edges[3]   =  y_ctb == (s->sps->pic_height_in_ctbs - 1);
    classes[0] = 0;
    
    if(!edges[0]) {
        sao[class]= &CTB(s->sao, x_ctb-1, y_ctb);
        classes[class] = 2;
        class++;
        x_shift = 8;
    }
    if(!edges[1]) {
        sao[class]= &CTB(s->sao, x_ctb, y_ctb-1);
        classes[class] = 1;
        class++;
        y_shift = 4;
        if(!edges[0]) {
            classes[class] = 3;
            sao[class]= &CTB(s->sao, x_ctb-1, y_ctb-1);
            class++;
        }
    }
    for(c_idx=0; c_idx<3; c_idx++)  {
        int chroma = c_idx ? 1 : c_idx;
        int x0 = x>>chroma;
        int y0 = y>>chroma;
        int stride = s->frame->linesize[c_idx];
        int ctb_size = (1 << (s->sps->log2_ctb_size)) >> s->sps->hshift[c_idx];
        int width = FFMIN(ctb_size,
                          (s->sps->pic_width_in_luma_samples >> s->sps->hshift[c_idx]) - x0);
        int height = FFMIN(ctb_size,
                           (s->sps->pic_height_in_luma_samples >> s->sps->vshift[c_idx]) - y0);
        
        uint8_t *src = &s->frame->data[c_idx][y0 * stride + x0];
        uint8_t *dst = &s->sao_frame->data[c_idx][y0 * stride + x0];
        int offset = (y_shift>>chroma) * stride + (x_shift>>chroma);
        
        copy_CTB(dst-offset, src-offset, edges[2] ? (width+(x_shift>>chroma)):width , edges[3] ? height+(y_shift>>chroma):height, stride );
        for(class_index = 0; class_index < class && c_idx>=c_idx_min && c_idx<c_idx_max; class_index++)    {
            switch (sao[class_index]->type_idx[c_idx]) {
                case SAO_BAND:
                    s->hevcdsp.sao_band_filter_wpp[ classes[class_index] ](dst, src, stride,  sao[class_index], edges, width, height, c_idx);
                    
                    break;
                case SAO_EDGE: {
                    s->hevcdsp.sao_edge_filter_wpp[ classes[class_index] ](dst, src, stride, sao[class_index],  edges, width, height, c_idx);
                    break;
                }
            }
        }
    }
}

void ff_hevc_deblocking_filter_CTB(HEVCContext *s, int x0, int y0)
{
    uint8_t *src;
    int x, y;
    int pixel = 1 + !!(s->sps->bit_depth - 8); // sizeof(pixel)
    
    int pic_width_in_min_pu = s->sps->pic_width_in_min_cbs * 4;
    int min_pu_size = 1 << (s->sps->log2_min_pu_size - 1);
    int log2_min_pu_size = s->sps->log2_min_pu_size - 1;
    int log2_ctb_size =  s->sps->log2_ctb_size;
    int x_end, y_end;
    int ctb_size = 1<<log2_ctb_size;
    x_end = x0+ctb_size;
    if (x_end > s->sps->pic_width_in_luma_samples)
        x_end = s->sps->pic_width_in_luma_samples;
    y_end = y0+ctb_size ;
    if (y_end > s->sps->pic_height_in_luma_samples)
        y_end = s->sps->pic_height_in_luma_samples;
    
    // vertical filtering
    for (x = x0 ? x0:8; x < x_end; x += 8) {
        for (y = y0; y < y_end; y += 4) {
            int bs = s->vertical_bs[(x >> 3) + (y >> 2) * s->bs_width];
            if (bs) {
                int qp = (get_qPy(s, x - 1, y) + get_qPy(s, x, y) + 1) >> 1;
                const int idxb = av_clip_c(qp + ((s->sh.beta_offset >> 1) << 1), 0, MAX_QP);
                const int beta = betatable[idxb];
                int no_p = 0;
                int no_q = 0;
                const int idxt = av_clip_c(qp + DEFAULT_INTRA_TC_OFFSET * (bs - 1) + ((s->sh.tc_offset >> 1) << 1), 0, MAX_QP + DEFAULT_INTRA_TC_OFFSET);
                const int tc = tctable[idxt];
                if(s->sps->pcm_enabled_flag && s->sps->pcm.loop_filter_disable_flag) {
                    int y_pu = y >> log2_min_pu_size;
                    int xp_pu = (x - 1) / min_pu_size;
                    int xq_pu = x >> log2_min_pu_size;
                    if (s->is_pcm[y_pu * pic_width_in_min_pu + xp_pu])
                        no_p = 1;
                    if (s->is_pcm[y_pu * pic_width_in_min_pu + xq_pu])
                        no_q = 1;
                }
                src = &s->frame->data[LUMA][y * s->frame->linesize[LUMA] + x];
                s->hevcdsp.hevc_loop_filter_luma(src, pixel, s->frame->linesize[LUMA], no_p, no_q, beta, tc);
                
                if ((x & 15) == 0 && (y & 7) == 0 && bs == 2) {
                    src = &s->frame->data[CB][(y / 2) * s->frame->linesize[CB] + (x / 2)];
                    s->hevcdsp.hevc_loop_filter_chroma(src, pixel, s->frame->linesize[CB], no_p, no_q, chroma_tc(s, qp, CB));
                    src = &s->frame->data[CR][(y / 2) * s->frame->linesize[CR] + (x / 2)];
                    s->hevcdsp.hevc_loop_filter_chroma(src, pixel, s->frame->linesize[CR], no_p, no_q, chroma_tc(s, qp, CR));
                }
            }
        }
    }
    // horizontal filtering
    if (x_end != s->sps->pic_width_in_luma_samples)
        x_end -= 8;
    for (y = y0 ? y0:8; y < y_end; y += 8) {
        int yp_pu = (y - 1) / min_pu_size;
        int yq_pu = y >> log2_min_pu_size;
        for (x = x0 ? x0-8:0; x < x_end; x += 4) {
            int bs = s->horizontal_bs[(x + y * s->bs_width) >> 2];
            if (bs) {
                int qp = (get_qPy(s, x, y - 1) + get_qPy(s, x, y) + 1) >> 1;
                const int idxb = av_clip_c(qp + ((s->sh.beta_offset >> 1) << 1), 0, MAX_QP);
                const int beta = betatable[idxb];
                int no_p = 0;
                int no_q = 0;
                const int idxt = av_clip_c(qp + DEFAULT_INTRA_TC_OFFSET * (bs - 1) + ((s->sh.tc_offset >> 1) << 1), 0, MAX_QP + DEFAULT_INTRA_TC_OFFSET);
                const int tc = tctable[idxt];
                if(s->sps->pcm_enabled_flag && s->sps->pcm.loop_filter_disable_flag) {
                    int x_pu = x >> log2_min_pu_size;
                    if (s->is_pcm[yp_pu * pic_width_in_min_pu + x_pu])
                        no_p = 1;
                    if (s->is_pcm[yq_pu * pic_width_in_min_pu + x_pu])
                        no_q = 1;
                }
                src = &s->frame->data[LUMA][y * s->frame->linesize[LUMA] + x];
                s->hevcdsp.hevc_loop_filter_luma(src, s->frame->linesize[LUMA], pixel, no_p, no_q, beta, tc);
                if ((x & 7) == 0 && (y & 15) == 0 && bs == 2) {
                    src = &s->frame->data[CB][(y / 2) * s->frame->linesize[CB] + (x / 2)];
                    s->hevcdsp.hevc_loop_filter_chroma(src, s->frame->linesize[CB], pixel, no_p, no_q, chroma_tc(s, qp, CB));
                    src = &s->frame->data[CR][(y / 2) * s->frame->linesize[CR] + (x / 2)];
                    s->hevcdsp.hevc_loop_filter_chroma(src, s->frame->linesize[CR], pixel, no_p, no_q, chroma_tc(s, qp, CR));
                }
            }
        }
    }
}


static int boundary_strength(HEVCContext *s, MvField *curr, uint8_t curr_cbf_luma, MvField *neigh, uint8_t neigh_cbf_luma, int tu_border)
{
    if (tu_border) {
        if (curr->is_intra || neigh->is_intra)
            return 2;
        if (curr_cbf_luma || neigh_cbf_luma)
            return 1;
    }

    if (s->sh.slice_type == P_SLICE) {
        if (abs(neigh->mv[0].x - curr->mv[0].x) >= 4 || abs(neigh->mv[0].y - curr->mv[0].y) >= 4 ||
            s->ref->refPicList[0].list[neigh->ref_idx[0]] != s->ref->refPicList[0].list[curr->ref_idx[0]])
            return 1;
        else
            return 0;
    } else if (s->sh.slice_type == B_SLICE) {
        int mvs = curr->pred_flag[0] + curr->pred_flag[1];
        if (mvs == neigh->pred_flag[0] + neigh->pred_flag[1]) {
            if (mvs == 2) {
                // same L0 and L1
                if (s->ref->refPicList[0].list[curr->ref_idx[0]] == s->ref->refPicList[0].list[neigh->ref_idx[0]]
                    && s->ref->refPicList[0].list[curr->ref_idx[0]] == s->ref->refPicList[1].list[curr->ref_idx[1]]
                    && s->ref->refPicList[0].list[neigh->ref_idx[0]] == s->ref->refPicList[1].list[neigh->ref_idx[1]]) {
                    if ((abs(neigh->mv[0].x - curr->mv[0].x) >= 4 || abs(neigh->mv[0].y - curr->mv[0].y) >= 4 ||
                        abs(neigh->mv[1].x - curr->mv[1].x) >= 4 || abs(neigh->mv[1].y - curr->mv[1].y) >= 4) &&
                        (abs(neigh->mv[1].x - curr->mv[0].x) >= 4 || abs(neigh->mv[1].y - curr->mv[0].y) >= 4 ||
                        abs(neigh->mv[0].x - curr->mv[1].x) >= 4 || abs(neigh->mv[0].y - curr->mv[1].y) >= 4))
                        return 1;
                    else
                        return 0;
                }
                else if (s->ref->refPicList[0].list[neigh->ref_idx[0]] == s->ref->refPicList[0].list[curr->ref_idx[0]]
                         && s->ref->refPicList[1].list[neigh->ref_idx[1]] == s->ref->refPicList[1].list[curr->ref_idx[1]]) {
                    if (abs(neigh->mv[0].x - curr->mv[0].x) >= 4 || abs(neigh->mv[0].y - curr->mv[0].y) >= 4 ||
                        abs(neigh->mv[1].x - curr->mv[1].x) >= 4 || abs(neigh->mv[1].y - curr->mv[1].y) >= 4)
                        return 1;
                    else
                        return 0;
                }
                else if (s->ref->refPicList[1].list[neigh->ref_idx[1]] == s->ref->refPicList[0].list[curr->ref_idx[0]]
                        && s->ref->refPicList[0].list[neigh->ref_idx[0]] == s->ref->refPicList[1].list[curr->ref_idx[1]]) {
                    if (abs(neigh->mv[1].x - curr->mv[0].x) >= 4 || abs(neigh->mv[1].y - curr->mv[0].y) >= 4 ||
                        abs(neigh->mv[0].x - curr->mv[1].x) >= 4 || abs(neigh->mv[0].y - curr->mv[1].y) >= 4)
                        return 1;
                    else
                        return 0;
                } else {
                    return 1;
                }
            } else { // 1 MV
                Mv A, B;
                int ref_A;
                int ref_B;
                if (curr->pred_flag[0]) {
                    A = curr->mv[0];
                    ref_A = s->ref->refPicList[0].list[curr->ref_idx[0]];
                }
                else {
                    A = curr->mv[1];
                    ref_A = s->ref->refPicList[1].list[curr->ref_idx[1]];
                }
                if (neigh->pred_flag[0]) {
                    B = neigh->mv[0];
                    ref_B = s->ref->refPicList[0].list[neigh->ref_idx[0]];
                } else {
                    B = neigh->mv[1];
                    ref_B = s->ref->refPicList[1].list[neigh->ref_idx[1]];
                }
                if (ref_A == ref_B) {
                    if (abs(A.x - B.x) >= 4 || abs(A.y - B.y) >= 4)
                        return 1;
                    else
                        return 0;
                } else
                    return 1;
            }
        }
        else
            return 1;
    }
    return 0;
}

void ff_hevc_deblocking_boundary_strengths(HEVCContext *s, int x0, int y0, int log2_trafo_size)
{
    int log2_min_pu_size = s->sps->log2_min_pu_size;
    int min_pu_size = 1 << s->sps->log2_min_pu_size;
    int pic_width_in_min_pu = s->sps->pic_width_in_min_cbs * 4;
    int i, j;
    int bs;
    MvField *tab_mvf = s->ref->tab_mvf;
    if ((y0 & 7) == 0) {
        int yp_pu = (y0 - 1) / min_pu_size;
        int yq_pu = y0 >> log2_min_pu_size;
        for (i = 0; i < (1<<log2_trafo_size); i+=4) {
            int x_pu = (x0 + i) >> log2_min_pu_size;
            MvField *top = &tab_mvf[yp_pu * pic_width_in_min_pu + x_pu];
            MvField *curr = &tab_mvf[yq_pu * pic_width_in_min_pu + x_pu];
            uint8_t top_cbf_luma = s->cbf_luma[yp_pu * pic_width_in_min_pu + x_pu];
            uint8_t curr_cbf_luma = s->cbf_luma[yq_pu * pic_width_in_min_pu + x_pu];
            bs = boundary_strength(s, curr, curr_cbf_luma, top, top_cbf_luma, 1);
            if (bs)
                s->horizontal_bs[((x0 + i) + y0 * s->bs_width) >> 2] = bs;
        }
    }
    // bs for TU internal horizontal PU boundaries
    if (log2_trafo_size > s->sps->log2_min_pu_size && s->sh.slice_type != I_SLICE)
        for (j = 8; j < (1<<log2_trafo_size); j += 8) {
            int yp_pu = (y0 + j - 1) >> log2_min_pu_size;
            int yq_pu = (y0 + j) >> log2_min_pu_size;
            for (i = 0; i < (1<<log2_trafo_size); i += 4) {
                int x_pu = (x0 + i) >> log2_min_pu_size;
                MvField *top = &tab_mvf[yp_pu * pic_width_in_min_pu + x_pu];
                MvField *curr = &tab_mvf[yq_pu * pic_width_in_min_pu + x_pu];
                uint8_t top_cbf_luma = s->cbf_luma[yp_pu * pic_width_in_min_pu + x_pu];
                uint8_t curr_cbf_luma = s->cbf_luma[yq_pu * pic_width_in_min_pu + x_pu];
                bs = boundary_strength(s, curr, curr_cbf_luma, top, top_cbf_luma, 0);
                if (bs)
                    s->horizontal_bs[((x0 + i) + (y0 + j) * s->bs_width) >> 2] = bs;
            }
        }
    // bs for vertical TU boundaries
    if ((x0 & 7) == 0) {
        int xp_pu = (x0 - 1) / min_pu_size;
        int xq_pu = x0 >> log2_min_pu_size;
        for (i = 0; i < (1<<log2_trafo_size); i+=4) {
            int y_pu = (y0 + i) >> log2_min_pu_size;
            MvField *left = &tab_mvf[y_pu * pic_width_in_min_pu + xp_pu];
            MvField *curr = &tab_mvf[y_pu * pic_width_in_min_pu + xq_pu];
            uint8_t left_cbf_luma = s->cbf_luma[y_pu * pic_width_in_min_pu + xp_pu];
            uint8_t curr_cbf_luma = s->cbf_luma[y_pu * pic_width_in_min_pu + xq_pu];
            bs = boundary_strength(s, curr, curr_cbf_luma, left, left_cbf_luma, 1);
            if (bs)
                s->vertical_bs[(x0 >> 3) + ((y0 + i) >> 2) * s->bs_width] = bs;
        }
    }
    // bs for TU internal vertical PU boundaries
    if (log2_trafo_size > s->sps->log2_min_pu_size && s->sh.slice_type != I_SLICE)
        for (j = 0; j < (1<<log2_trafo_size); j += 4) {
            int y_pu = (y0 + j) >> log2_min_pu_size;
            for (i = 8; i < (1<<log2_trafo_size); i += 8) {
                int xp_pu = (x0 + i - 1) >> log2_min_pu_size;
                int xq_pu = (x0 + i) >> log2_min_pu_size;
                MvField *left = &tab_mvf[y_pu * pic_width_in_min_pu + xp_pu];
                MvField *curr = &tab_mvf[y_pu * pic_width_in_min_pu + xq_pu];
                uint8_t left_cbf_luma = s->cbf_luma[y_pu * pic_width_in_min_pu + xp_pu];
                uint8_t curr_cbf_luma = s->cbf_luma[y_pu * pic_width_in_min_pu + xq_pu];
                bs = boundary_strength(s, curr, curr_cbf_luma, left, left_cbf_luma, 0);
                if (bs)
                    s->vertical_bs[((x0 + i) >> 3) + ((y0 + j) >> 2) * s->bs_width] = bs;
            }
        }
}
#undef LUMA
#undef CB
#undef CR

void hls_filter(HEVCContext *s, int x, int y)
{
    int c_idx_min = s->sh.slice_sample_adaptive_offset_flag[0] != 0 ? 0 : 1;
    int c_idx_max = s->sh.slice_sample_adaptive_offset_flag[1] != 0 ? 3 : 1;
    if(!s->sh.disable_deblocking_filter_flag)
        ff_hevc_deblocking_filter_CTB(s, x, y);
    if(s->sps->sample_adaptive_offset_enabled_flag)
        ff_hevc_sao_filter_CTB(s, x, y, c_idx_min, c_idx_max);
}
void hls_filters(HEVCContext *s, int x_ctb, int y_ctb, int ctb_size)
{
    if(y_ctb && x_ctb) {
        hls_filter(s, x_ctb-ctb_size, y_ctb-ctb_size);
        if(x_ctb >= (s->sps->pic_width_in_luma_samples - ctb_size))
            hls_filter(s, x_ctb, y_ctb-ctb_size);
        if(y_ctb >= (s->sps->pic_height_in_luma_samples - ctb_size))
            hls_filter(s, x_ctb-ctb_size, y_ctb);
    }
}
