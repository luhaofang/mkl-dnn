/*******************************************************************************
* Copyright 2016 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "c_types_map.hpp"
#include "nstl.hpp"
#include "type_helpers.hpp"
#include "utils.hpp"

#include "jit_avx2_conv_kernel_f32.hpp"

#define GET_OFF(field) offsetof(jit_conv_call_s, field)

namespace mkldnn {
namespace impl {
namespace cpu {

using namespace mkldnn::impl::memory_format;
using namespace mkldnn::impl::utils;


void jit_avx2_conv_fwd_kernel_f32::oh_step_unroll_kw(int ur_w, int pad_l,
        int pad_r) {
    using Xbyak::Ymm;

    int iw = jcp.iw;
    int ih = jcp.ih;
    int kw = jcp.kw;
    int kh = jcp.kh;
    int nb_ic = jcp.nb_ic;
    int stride_w = jcp.stride_w;
    int nb_oc_block = jcp.nb_oc_blocking;
    int ic_blk = jcp.ic_block;
    int oc_blk = jcp.oc_block;

    for (int ki = 0; ki < kw; ki++) {
        int jj_start = nstl::max(0, (pad_l - ki + stride_w - 1)/stride_w);
        int jj_end = ur_w - nstl::max(0, (ki + pad_r - (kw - 1) + stride_w - 1)/stride_w);
        for (int ifm2 = 0; ifm2 < ic_blk; ifm2++) {
            for (int jj = jj_start; jj < jj_end; jj++) {
                int inp_off;
                if (jcp.src_fmt == nchw)
                    inp_off = ifm2 * ih * iw + (ki + jj * stride_w - pad_l);
                else
                    inp_off = (ki + jj * stride_w - pad_l) * ic_blk + ifm2;
                vbroadcastss(Ymm(nb_oc_block * ur_w + jj),
                        ptr[aux_reg_input + sizeof(float) * inp_off]);
            }
            for (int ii = 0; ii < nb_oc_block; ii++) {
                int ker_off = ii * nb_ic * kh * kw * ic_blk * oc_blk
                        + ki * ic_blk * oc_blk + ifm2 * oc_blk;
                vmovups(ymm15, ptr[aux_reg_kernel + sizeof(float) * ker_off]);
                for (int jj = jj_start; jj < jj_end; jj++)
                    vfmadd231ps(Ymm(ur_w * ii + jj),
                            Ymm(nb_oc_block * ur_w + jj), ymm15);
            }
        }
    }
}

void jit_avx2_conv_fwd_kernel_f32::oh_step_nopad(int ur_w, int pad_l, int pad_r,
        char pad_label) {
    using Xbyak::Ymm;
    char kw_label[4] = ".wP";
    kw_label[2] = pad_label;

    int iw = jcp.iw;
    int ih = jcp.ih;
    int kw = jcp.kw;
    int kh = jcp.kh;
    int nb_ic = jcp.nb_ic;
    int stride_w = jcp.stride_w;
    int nb_oc_block = jcp.nb_oc_blocking;
    int ic_blk = jcp.ic_block;
    int oc_blk = jcp.oc_block;

    xor_(ki_iter, ki_iter);
    L(kw_label);
    {
        int jj_start = 0;
        int jj_end = ur_w;
        for (int ifm2 = 0; ifm2 < ic_blk; ifm2++) {
            for (int jj = jj_start; jj < jj_end; jj++) {
                int inp_off;
                if (jcp.src_fmt == nchw)
                    inp_off = ifm2 * ih * iw + (jj * stride_w - pad_l);
                else
                    inp_off = (jj * stride_w - pad_l) * ic_blk + ifm2;
                vbroadcastss(Ymm(nb_oc_block * ur_w + jj),
                        ptr[aux_reg_input + sizeof(float) * inp_off]);
            }
            for (int ii = 0; ii < nb_oc_block; ii++) {
                int aux_kernel_offset = ii * nb_ic * kh * kw * ic_blk * oc_blk
                    + ifm2 * oc_blk;
                vmovups(ymm15, ptr[aux_reg_kernel
                        + sizeof(float) * aux_kernel_offset]);
                for (int jj = jj_start; jj < jj_end; jj++)
                    vfmadd231ps(Ymm(ur_w * ii + jj),
                            Ymm(nb_oc_block * ur_w + jj), ymm15);
            }
        }
        add(aux_reg_kernel, sizeof(float) * oc_blk * ic_blk);
        add(aux_reg_input, sizeof(float) * (jcp.src_fmt == nchw ? 1 : ic_blk));

        inc(ki_iter);
        cmp(ki_iter, kw);
        jl(kw_label, T_NEAR);
    }
}

void jit_avx2_conv_fwd_kernel_f32::width_blk_step(int ur_w, int pad_l, int pad_r,
        char pad_label) {
    using Xbyak::Ymm;

    int iw = jcp.iw;
    int kw = jcp.kw;
    int ow = jcp.ow;
    int oh = jcp.oh;
    int nb_oc_block = jcp.nb_oc_blocking;
    int ic_blk = jcp.ic_block;
    int oc_blk = jcp.oc_block;
    const int inp_mult = jcp.src_fmt == nchw ? 1 : ic_blk;

    char init_done_label[4] = {'.', 'i', pad_label, '\0'};
    char init_first_label[4] = {'.', 'f', pad_label, '\0'};

    test(reg_ci_flag, IC_FLAG_FIRST);
    jne(init_first_label, T_NEAR);

    for (int ii = 0; ii < nb_oc_block; ii++)
        for (int jj = 0; jj < ur_w; jj++)
            vmovups(Ymm(ur_w * ii + jj), YWORD[reg_output
                    + sizeof(float) * (ii * oh * ow + jj) * oc_blk]);
    jmp(init_done_label);

    L(init_first_label);
    if (this->jcp.with_bias) {
        for (int ii = 0; ii < nb_oc_block; ii++)
            for (int jj = 0; jj < ur_w; jj++)
                vmovups(Ymm(ur_w * ii + jj),
                        YWORD[reg_bias + sizeof(float)*ii*oc_blk]);
    } else {
        for (int ii = 0; ii < nb_oc_block; ii++)
            for (int jj = 0; jj < ur_w; jj++)
                vpxor(Ymm(ur_w * ii + jj), Ymm(ur_w * ii + jj));
    }

    L(init_done_label);

    mov(aux_reg_input, reg_input);
    mov(aux_reg_kernel, reg_kernel);

    mov(kj, reg_kh);
    char kh_label[4] = {'.', 'h', pad_label, '\0'};
    L(kh_label);
    {
        if (jcp.kw >= 5 && pad_l == 0 && pad_r == 0) {
            oh_step_nopad(ur_w, pad_l, pad_r, pad_label);
            sub(aux_reg_input, sizeof(float) * kw * inp_mult);
            add(aux_reg_input, sizeof(float) * iw * inp_mult);
        } else {
            oh_step_unroll_kw(ur_w, pad_l, pad_r);
            add(aux_reg_kernel, sizeof(float) * kw * oc_blk * ic_blk);
            add(aux_reg_input, sizeof(float) * iw * inp_mult);
        }

        dec(kj);
        cmp(kj, 0);
        jg(kh_label, T_NEAR);
    }

    char done_label[4] = {'.', 'd', pad_label, '\0'};
    char regular_store_label[4] = {'.', 's', pad_label, '\0'};
    if (this->jcp.with_relu) {
        assert(nb_oc_block*ur_w < 15);
        test(reg_ci_flag, IC_FLAG_LAST);
        je(regular_store_label, T_NEAR);

        Ymm yzero = ymm15, ymask = ymm14;
        vxorps(yzero, yzero, yzero);
        for (int ii = 0; ii < nb_oc_block; ii++) {
            for (int jj = 0; jj < ur_w; jj++) {
                const size_t o_off = (ii * oh * ow + jj) * oc_blk;
                Ymm reg_out = Ymm(ur_w * ii + jj);

                vcmpgtps(ymask, reg_out, yzero);
                vblendvps(reg_out, yzero, reg_out, ymask);
                vmovups(YWORD[reg_output + sizeof(float) * o_off], reg_out);
            }
        }

        jmp(done_label);
        L(regular_store_label);
    }
    for (int ii = 0; ii < nb_oc_block; ii++) {
        for (int jj = 0; jj < ur_w; jj++) {
            const size_t o_off = (ii * oh * ow + jj) * oc_blk;
            Ymm reg_out = Ymm(ur_w * ii + jj);
            vmovups(YWORD[reg_output + sizeof(float) * o_off], reg_out);
        }
    }
    L(done_label);
}

void jit_avx2_conv_fwd_kernel_f32::generate() {
    using Xbyak::Ymm;
    this->preamble();

    mov(reg_input, ptr[this->param1 + GET_OFF(src)]);
    mov(reg_output, ptr[this->param1 + GET_OFF(dst)]);
    mov(reg_kernel, ptr[this->param1 + GET_OFF(filt)]);
    if (jcp.with_bias)
        mov(reg_bias, ptr[this->param1 + GET_OFF(bias)]);
    mov(reg_kh, ptr[this->param1 + GET_OFF(kh_padding)]);
    mov(reg_ci_flag, ptr[this->param1 + GET_OFF(ic_flag)]);

    // NB: works only for jcp.ur_w == 3 && jcp.nb_oc % 4 == 0
    int ur_w = jcp.ur_w;
    int ur_w_tail = jcp.ur_w_tail;
    int n_oi = jcp.ow / ur_w;
    int iw = jcp.iw;
    int kw = jcp.kw;
    int ic_blk = jcp.ic_block;
    int oc_blk = jcp.oc_block;
    int str_w = jcp.stride_w;
    const int inp_mult = jcp.src_fmt == nchw ? 1 : ic_blk;

    int l_pad = jcp.l_pad;
    int r_pad = nstl::max(0, (int(jcp.ow) - 1) * str_w + kw - 1
            - (iw + l_pad - 1));
    int r_pad1 = (ur_w * n_oi - 1) * str_w + kw - 1 - (iw + l_pad - 1);
    if (r_pad1 > 0) n_oi--;

    if (l_pad > 0) {
        n_oi--;
        if (n_oi < 0 && r_pad1 > 0) {
            width_blk_step(ur_w, l_pad, r_pad1, 'l'); // "lrpad"
        } else {
            width_blk_step(ur_w, l_pad, 0, 'l'); // "lpad"
        }
        add(reg_input, sizeof(float) * (ur_w * str_w - l_pad) * inp_mult);
        add(reg_output, sizeof(float) * ur_w * oc_blk);
    }

    xor_(oi_iter, oi_iter);
    if (n_oi > 0) {
        L(".ow_loop");

        width_blk_step(ur_w, 0, 0, 'm'); // "middle"
        add(reg_input, sizeof(float) * ur_w * str_w * inp_mult);
        add(reg_output, sizeof(float) * ur_w * oc_blk);

        inc(oi_iter);
        cmp(oi_iter, n_oi);
        jl(".ow_loop", T_NEAR);
    }

    if (r_pad1 > 0 && n_oi >=0) {
        width_blk_step(ur_w, 0, r_pad1, 'r'); // "rpad"
        add(reg_input, sizeof(float) * ur_w * str_w * inp_mult);
        add(reg_output, sizeof(float) * ur_w * oc_blk);
    }

    if (ur_w_tail != 0)
        width_blk_step(ur_w_tail, 0, r_pad, 't'); // "tail"

    this->postamble();
}

status_t jit_avx2_conv_fwd_kernel_f32::init_conf(jit_conv_conf_t &jcp,
        const convolution_desc_t &cd, const memory_desc_wrapper &src_d,
        const memory_desc_wrapper &weights_d, const memory_desc_wrapper &dst_d,
        bool with_relu, double relu_negative_slope)
{
    const bool with_groups = weights_d.ndims() == src_d.ndims() + 1;

    jcp.ngroups = with_groups ? weights_d.dims()[0] : 1;
    jcp.mb = src_d.dims()[0];

    jcp.oc = dst_d.dims()[1] / jcp.ngroups;
    jcp.ic = src_d.dims()[1] / jcp.ngroups;

    jcp.ih = src_d.dims()[2];
    jcp.iw = src_d.dims()[3];
    jcp.oh = dst_d.dims()[2];
    jcp.ow = dst_d.dims()[3];

    jcp.kh = weights_d.dims()[with_groups + 2];
    jcp.kw = weights_d.dims()[with_groups + 3];

    jcp.t_pad = cd.padding[0][0];
    jcp.l_pad = cd.padding[0][1];

    jcp.stride_h = cd.strides[0];
    jcp.stride_w = cd.strides[1];

    jcp.src_fmt = src_d.format();
    jcp.with_bias = cd.bias_desc.format != memory_format::undef;
    jcp.with_relu = with_relu;
    jcp.relu_negative_slope = relu_negative_slope;

    const bool flat = jcp.ic == 3;
    const bool mimo = !flat;

    bool args_ok = true
        && implication(flat, one_of(src_d.format(), nchw, nhwc))
        && implication(mimo, src_d.format() == nChw8c)
        && weights_d.format() ==
                (with_groups ? gOIhw8i8o : (flat ? Ohwi8o : OIhw8i8o))
        && one_of(cd.bias_desc.format, memory_format::undef, any, x)
        && dst_d.format() == nChw8c;
    if (!args_ok) return status::unimplemented;

    const int simd_w = 8;

    jcp.ur_h = 1; /* no code-unrolling by h so far */
    jcp.ur_w = 3;
    if (jcp.ow < jcp.ur_w) jcp.ur_w = jcp.ow;
    jcp.ur_w_tail = jcp.ow % jcp.ur_w;

    args_ok = true
        && jcp.oc % simd_w == 0
        && jcp.l_pad <= jcp.ur_w
        && implication(jcp.kw > 7, (jcp.t_pad == 0 && jcp.l_pad == 0)
                || (jcp.stride_w == 1 && jcp.stride_h == 1))
        && implication(mimo, jcp.ic % simd_w == 0);
    if (!args_ok) return status::unimplemented;

    int r_pad_no_tail = nstl::max(0,
            (jcp.ow - jcp.ur_w_tail - 1) * jcp.stride_w + (jcp.kw - 1)
            - (jcp.iw + jcp.l_pad - 1));

    /* maximum 1 ur_w block with r_pad so far */
    if (r_pad_no_tail > jcp.ur_w) return status::unimplemented;

    jcp.ic_block = (jcp.ic % simd_w != 0) ? jcp.ic : simd_w;
    jcp.nb_ic = jcp.ic / jcp.ic_block;

    jcp.oc_block = simd_w;
    jcp.nb_oc = jcp.oc / jcp.oc_block;
    jcp.nb_ic_blocking =  jcp.nb_oc_blocking = 1;
    for (int b = 4; b > 1; b--) {
        if (jcp.nb_oc % b == 0) {
            jcp.nb_oc_blocking = b;
            break;
        }
    }

    return status::success;
}

inline Xbyak::Address jit_avx2_conv_bwd_data_kernel_f32::get_address(
        Xbyak::Reg64 base, int offset) {
    using Xbyak::Ymm;
    return YWORD[base + offset];
}

void jit_avx2_conv_bwd_data_kernel_f32::hsw_iter_s1(int ur_w, int l_overflow,
        int r_overflow, const char* kh_lable) {
    using Xbyak::Ymm;

    int kw    = jcp.kw;
    int kh    = jcp.kh;
    int iw    = jcp.iw;
    int ih    = jcp.ih;
    int ow    = jcp.ow;

    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;
    int nb_ic_block = jcp.nb_ic_blocking;

    for (int ii = 0; ii < nb_ic_block; ii++)
        for (int jj = 0; jj < ur_w; jj++)
            vmovups(Ymm(ur_w*ii+jj), ptr [ reg_dsrc + sizeof(float)*(ii*ih*iw+jj)*ic_block ]);

    mov(aux_reg_ddst, reg_ddst);
    mov(aux_reg_kernel, reg_kernel);

    mov(kj, reg_kh);
    L(kh_lable);
    {
        //TODO: try lable
        for (int ki = 0; ki < kw; ki++)
        {
            int jj_start = nstl::max(0, l_overflow - (kw-1) + ki) ; // 0;
            int jj_end = ur_w - nstl::max(0, r_overflow - ki); // ur_w;
            for (int ofm2 = 0; ofm2 < jcp.oc_block; ofm2++)
            {
                for (int jj =jj_start ; jj < jj_end; jj++)
                {
                    int aux_output_offset = (jj + jcp.l_pad - ki)*jcp.oc_block + ofm2;
                    vbroadcastss(Ymm(nb_ic_block*ur_w+jj), ptr [ aux_reg_ddst + sizeof(float)*aux_output_offset ]);
                }
                for (int ii = 0; ii  < nb_ic_block; ii++)
                {
                    int aux_kernel_offset = ii*kh*kw*jcp.ic_block*jcp.oc_block +
                                             ki*jcp.ic_block*jcp.oc_block+ ofm2*jcp.ic_block;
                    vmovups(ymm15, ptr [ aux_reg_kernel +  sizeof(float)*aux_kernel_offset ]);
                    for (int jj = jj_start; jj  < jj_end; jj++)
                    {
                        vfmadd231ps(Ymm(ur_w*ii+jj), Ymm(nb_ic_block*ur_w+jj), ymm15);
                    }
                }
            }
        }
        add(aux_reg_kernel, sizeof(float)*kw *oc_block*ic_block);
        sub(aux_reg_ddst, sizeof(float)*ow*oc_block);

        dec(kj);
        cmp(kj, 0);
        jg(kh_lable, T_NEAR);
    }

    for (int ii = 0; ii < nb_ic_block; ii++)
        for (int jj = 0; jj < ur_w; jj++)
            vmovups(ptr [ reg_dsrc + sizeof(float)*(ii*ih*iw+jj)*ic_block ], Ymm(ur_w*ii+jj));

}

void jit_avx2_conv_bwd_data_kernel_f32::generate() {
    using Xbyak::Ymm;
    preamble();

    mov(reg_dsrc, ptr[this->param1 + GET_OFF(src)]);
    mov(reg_ddst, ptr[this->param1 + GET_OFF(dst)]);
    mov(reg_kernel, ptr[this->param1 + GET_OFF(filt)]);
    mov(reg_kh, ptr[this->param1 + GET_OFF(kh_padding)]);

    int n_oi = jcp.iw / jcp.ur_w;
    xor_(oi_iter, oi_iter);

    int l_overflow = nstl::max(0, jcp.kw - 1 - jcp.l_pad);
    if (l_overflow > 0)
    {
        hsw_iter_s1(jcp.ur_w, l_overflow, 0, ".kh_loop_oimain_overflow_l");
        add(reg_dsrc , sizeof(float)*jcp.ur_w*jcp.ic_block);
        add(reg_ddst, sizeof(float)*jcp.ur_w*jcp.oc_block);
        inc(oi_iter);
    }

    int r_pad = jcp.iwp - jcp.iw - jcp.l_pad;
    int r_overflow1 = nstl::max(0,jcp.kw - 1 - (jcp.iw - jcp.ur_w*n_oi) - r_pad);
    int r_overflow  = nstl::max(0,jcp.kw - 1 - r_pad);
    if (r_overflow1 > 0)
        n_oi--;

    if ((l_overflow <= 0 && n_oi > 0)
      ||(l_overflow >  0 && n_oi > 1))
    {
        L(".ow_loop");
        {
            hsw_iter_s1(jcp.ur_w, 0, 0, ".kh_loop_oimain");
            add(reg_dsrc, sizeof(float)*jcp.ur_w*jcp.ic_block);
            add(reg_ddst, sizeof(float)*jcp.ur_w*jcp.oc_block);

            inc(oi_iter);
            cmp(oi_iter, n_oi); jl(".ow_loop", T_NEAR);
        }
    }

    if (r_overflow1 > 0 )
    {
        hsw_iter_s1(jcp.ur_w, 0, r_overflow1, ".kh_loop_oimain_overflow_r");
        add(reg_dsrc, sizeof(float)*jcp.ur_w*jcp.ic_block);
        add(reg_ddst, sizeof(float)*jcp.ur_w*jcp.oc_block);
    }
    if (jcp.ur_w_tail != 0)
        hsw_iter_s1(jcp.ur_w_tail, 0, r_overflow, ".kh_loop_oitail");

    this->postamble();
}

status_t jit_avx2_conv_bwd_data_kernel_f32::init_conf(jit_conv_conf_t &jcp,
        const convolution_desc_t &cd, const memory_desc_wrapper &diff_src_d,
        const memory_desc_wrapper &weights_d,
        const memory_desc_wrapper &diff_dst_d)
{
    const bool with_groups = weights_d.ndims() == diff_src_d.ndims() + 1;

    jcp.ngroups = with_groups ? weights_d.dims()[0] : 1;
    jcp.mb = diff_src_d.dims()[0];

    jcp.oc = diff_dst_d.dims()[1] / jcp.ngroups;
    jcp.ic = diff_src_d.dims()[1] / jcp.ngroups;

    jcp.ih = diff_src_d.dims()[2];
    jcp.iw = diff_src_d.dims()[3];
    jcp.oh = diff_dst_d.dims()[2];
    jcp.ow = diff_dst_d.dims()[3];

    jcp.kh = weights_d.dims()[with_groups + 2];
    jcp.kw = weights_d.dims()[with_groups + 3];

    jcp.t_pad = cd.padding[0][0];
    jcp.l_pad = cd.padding[0][1];

    jcp.stride_h = cd.strides[0];
    jcp.stride_w = cd.strides[1];

    const int simd_w = 8;

    /* derivatives */
    jcp.ihp = jcp.ih + 2*jcp.t_pad;
    jcp.iwp = jcp.iw + 2*jcp.l_pad;
    jcp.ohp = jcp.oh; /* do we really need */
    jcp.owp = jcp.ow; /* padded output ??? */

    jcp.ic_block = (jcp.ic % simd_w) ? 1 : simd_w;
    jcp.nb_ic = jcp.ic / jcp.ic_block;

    jcp.oc_block = simd_w;
    if (jcp.oc % jcp.oc_block) return status::unimplemented;
    jcp.nb_oc = jcp.oc / jcp.oc_block;

    jcp.ur_h = 1; /* no code-unrolling by h so far */
    jcp.nb_ic_blocking = 1;
    jcp.nb_oc_blocking = 1;
    jcp.ur_w = 1;

    jcp.src_fmt = diff_src_d.format();

    bool args_ok = true
        && diff_src_d.format() == nChw8c
        && weights_d.format() == with_groups ? gOIhw8o8i : OIhw8o8i
        && diff_dst_d.format() == nChw8c
        && jcp.stride_w == jcp.stride_h
        && jcp.stride_w == 1
        && jcp.ic % simd_w == 0
        && jcp.oc % simd_w == 0
        && jcp.t_pad == jcp.l_pad /* Only AlexNet so far */
        && (jcp.t_pad == 1 || jcp.t_pad == 2) /* Only AlexNet so far */
        && jcp.oh == (jcp.ihp - jcp.kh) / jcp.stride_h + 1
        && jcp.ow == (jcp.iwp - jcp.kw) / jcp.stride_w + 1;
    if (!args_ok) return status::unimplemented;

    jcp.ur_w = 3;
    if (jcp.stride_w > 1 || jcp.stride_h > 1)
        return status::unimplemented;
    if (jcp.ngroups == 1
        && jcp.kw == 1 && jcp.kh == 1 && jcp.l_pad == 0 && jcp.t_pad == 0
        && jcp.stride_w == 1 && jcp.stride_h == 1
        && jcp.iw == jcp.ow && jcp.ih == jcp.oh
        && jcp.ic_block == simd_w) {

        jcp.nb_ic_blocking = 3;

        return status::success;
    }

    jcp.ur_w = 3;
    for (int b = 4; b > 1; b--)
    {
        if (jcp.nb_ic % b == 0)
        {
            jcp.nb_ic_blocking = b;
            break;
        }
    }
    jcp.ur_w_tail = jcp.iw % jcp.ur_w;
    int l_overflow = nstl::max(0, jcp.kw - 1 - jcp.l_pad);
    if (l_overflow > jcp.ur_w) /* maximum 1 step with l_overflow so far */
        return status::unimplemented;
    int r_pad = jcp.iwp - jcp.iw - jcp.l_pad;
    int r_overflow_step0 = nstl::max(0, jcp.kw - 1 - (jcp.iw - jcp.ur_w) - r_pad);
    if (l_overflow > 0 && r_overflow_step0 > 0) /* no steps with both left and
                                                   right overflow so far */
        return status::unimplemented;
    int r_overflow_no_tail = nstl::max(0,jcp.kw - 1 - jcp.ur_w_tail - r_pad);
    if (r_overflow_no_tail > jcp.ur_w) /* maximum 1 ur_w block with
                                          r_overflow so far */
        return status::unimplemented;
    return status::success;
}

void jit_avx2_conv_bwd_weights_kernel_f32::generate() {
    using Xbyak::Ymm;
    this->preamble();

    mov(reg_input, ptr[this->param1 + GET_OFF(src)]);
    mov(reg_output, ptr[this->param1 + GET_OFF(dst)]);
    mov(reg_kernel, ptr[this->param1 + GET_OFF(filt)]);

    compute_oh_loop_common();

    this->postamble();
}

status_t jit_avx2_conv_bwd_weights_kernel_f32::init_conf(jit_conv_conf_t &jcp,
        const convolution_desc_t &cd, const memory_desc_wrapper &src_d,
        const memory_desc_wrapper &diff_weights_d,
        const memory_desc_wrapper &diff_dst_d) {
    const bool with_groups = diff_weights_d.ndims() == src_d.ndims() + 1;

    jcp.ngroups = with_groups ? diff_weights_d.dims()[0] : 1;
    jcp.mb = src_d.dims()[0];

    jcp.oc = diff_dst_d.dims()[1] / jcp.ngroups;
    jcp.ic = src_d.dims()[1] / jcp.ngroups;

    jcp.ih = src_d.dims()[2];
    jcp.iw = src_d.dims()[3];
    jcp.oh = diff_dst_d.dims()[2];
    jcp.ow = diff_dst_d.dims()[3];

    jcp.kh = diff_weights_d.dims()[with_groups + 2];
    jcp.kw = diff_weights_d.dims()[with_groups + 3];

    jcp.t_pad = cd.padding[0][0];
    jcp.l_pad = cd.padding[0][1];

    jcp.stride_h = cd.strides[0];
    jcp.stride_w = cd.strides[1];

    jcp.src_fmt = src_d.format();
    jcp.with_bias = cd.diff_bias_desc.format != memory_format::undef;
    jcp.with_relu = 0;
    jcp.relu_negative_slope = 0;

    bool args_ok = true
        && src_d.format() == nChw8c
        && diff_weights_d.format() == (with_groups ? gOIhw8i8o : OIhw8i8o)
        && one_of(cd.bias_desc.format, memory_format::undef, x)
        && diff_dst_d.format() == nChw8c
        && jcp.kw < 14;
    if (!args_ok) return status::unimplemented;

    const int simd_w = 8;
    jcp.ic_block = simd_w;
    jcp.nb_ic = jcp.ic / jcp.ic_block;

    jcp.oc_block = simd_w;
    jcp.nb_oc = jcp.oc / jcp.oc_block;
    jcp.nb_ic_blocking = jcp.nb_oc_blocking = 1;
    return status::success;
}

inline Xbyak::Address jit_avx2_conv_bwd_weights_kernel_f32::get_address(
        Xbyak::Reg64 base, int offset) {
    using Xbyak::Ymm;
    return YWORD[base + offset];
}

inline void jit_avx2_conv_bwd_weights_kernel_f32::oh_step_comeback_pointers(
        const char *kh_comeback_label) {
    mov(kj, reg_kh);
    L(kh_comeback_label); {
        sub(reg_input, sizeof(float) * jcp.iw * jcp.ic_block);
        sub(reg_kernel, sizeof(float) * jcp.kw * jcp.ic_block * jcp.oc_block);
        dec(kj);
        cmp(kj, 0);
        jg(kh_comeback_label, T_NEAR);
    }
}

inline void jit_avx2_conv_bwd_weights_kernel_f32::compute_ic_block_step(
        int ur_w, int pad_l, int pad_r, int ic_block_step, int input_offset,
        int kernel_offset, int output_offset) {
    using Xbyak::Ymm;

    const int kw = jcp.kw;
    const int ic_block = jcp.ic_block;
    const int oc_block = jcp.oc_block;
    for (int i_kw = 0; i_kw < kw; i_kw++) {
        for (int i_ic = 0; i_ic < ic_block_step; i_ic++) {
            size_t off = sizeof(float)*(i_kw*ic_block + i_ic)*jcp.oc_block
                + kernel_offset;
            vmovups(Ymm(i_kw*ic_block_step + i_ic), get_address(reg_kernel,
                        off));
        }
    }

    for (int i_ur = 0; i_ur < ur_w; i_ur++) {
        vmovups(Ymm(kw*ic_block_step + 0), get_address(reg_output,
                    sizeof(float)*i_ur*oc_block + output_offset));

        for (int i_kw = 0; i_kw < kw; i_kw++) {
            int i_iw = i_ur*jcp.stride_w + i_kw;
            if (i_iw - pad_l < 0
                    || i_iw > (ur_w - 1)*jcp.stride_w + kw - 1 - pad_r)
                continue;
            for (int i_ic = 0; i_ic < ic_block_step; i_ic++) {
                size_t i_off = sizeof(float)*((i_iw - pad_l)*ic_block + i_ic)
                    + input_offset;
                vbroadcastss(Ymm(kw*ic_block_step + 1),
                        get_address(reg_input, i_off));
                vfmadd231ps(Ymm(i_kw * ic_block_step + i_ic),
                        Ymm(kw*ic_block_step + 0), Ymm(kw*ic_block_step + 1));
            }
        }
    }

    for (int i_kw = 0; i_kw < kw; i_kw++) {
        for (int i_ic = 0; i_ic < ic_block_step; i_ic++) {
            size_t off = sizeof(float)*(i_kw*ic_block + i_ic)*jcp.oc_block
                + kernel_offset;
            vmovups(get_address(reg_kernel, off), Ymm(i_kw*ic_block_step
                        + i_ic));
        }
    }
}

inline void jit_avx2_conv_bwd_weights_kernel_f32::compute_oh_step_disp(
        const char *kh_label, const char *ic_block_label,
        const char *ow_block_label, const char *kh_comeback_label)
{
    int ic_block_step = jcp.kw > 7 ? 1 : (jcp.kw > 3 ? 2 :
            (jcp.kw > 1 ? 4 : 8));
    const int max_ur_w = (jcp.ow > 56) ? 14 : 28;

    if (jcp.ow <= max_ur_w) {
        compute_oh_step_unroll_ow(kh_label, ic_block_label, ow_block_label,
                kh_comeback_label, ic_block_step, max_ur_w);
    } else {
        compute_oh_step_common(kh_label, ic_block_label, ow_block_label,
                kh_comeback_label, ic_block_step, max_ur_w);
    }
    oh_step_comeback_pointers(kh_comeback_label);
}

inline void jit_avx2_conv_bwd_weights_kernel_f32::compute_oh_step_unroll_ow(
        const char *kh_label, const char *ic_block_label,
        const char *ow_block_label, const char *kh_comeback_label,
        int ic_block_step, int max_ur_w) {
    UNUSED(ow_block_label);
    UNUSED(kh_comeback_label);
    UNUSED(max_ur_w);

    const int ic_block = jcp.ic_block;
    const int oc_block = jcp.oc_block;

    const int r_pad = nstl::max(0, (jcp.ow - 1)*jcp.stride_w + jcp.kw - jcp.iw
            - jcp.l_pad);

    mov(kj, reg_kh);
    L(kh_label); {
        xor_(b_ic, b_ic);
        L(ic_block_label); {
            compute_ic_block_step(jcp.ow, jcp.l_pad, r_pad, ic_block_step, 0,
                    0, 0);
            add(reg_input, sizeof(float)*ic_block_step);
            add(reg_kernel, sizeof(float)*ic_block_step*oc_block);
            add(b_ic, ic_block_step);
            cmp(b_ic, ic_block);
            jl(ic_block_label, T_NEAR);
        }

        add(reg_input, sizeof(float)*(jcp.iw - 1)*ic_block);
        add(reg_kernel, sizeof(float)*(jcp.kw - 1)*ic_block*oc_block);
        dec(kj);
        cmp(kj, 0);
        jg(kh_label, T_NEAR);
    }
}

inline void jit_avx2_conv_bwd_weights_kernel_f32::compute_oh_step_common(
        const char *kh_label, const char *ic_block_label,
        const char *ow_block_label, const char *kh_comeback_label,
        int ic_block_step, int max_ur_w) {
    UNUSED(kh_comeback_label);

    const int ic_block = jcp.ic_block;
    const int oc_block = jcp.oc_block;
    const int stride_w = jcp.stride_w;

    const int r_pad = nstl::max(0, (jcp.ow - 1)*jcp.stride_w + jcp.kw - jcp.iw
            - jcp.l_pad);

    int ur_w = nstl::min(jcp.ow, max_ur_w);
    int ur_w_trips = jcp.ow / ur_w;
    int ur_w_tail = jcp.ow % ur_w;
    if ((ur_w_tail == 0 && r_pad != 0) || r_pad >= ur_w_tail) {
        if (ur_w_trips > 1) {
            ur_w_tail += ur_w;
            ur_w_trips--;
        } else {
            ur_w_tail += (ur_w - ur_w / 2);
            ur_w = ur_w / 2;
        }
    }
    int input_comeback = (ur_w_trips*ur_w*stride_w - jcp.l_pad)*ic_block;
    int output_comeback = ur_w_trips*ur_w*oc_block;

    mov(kj, reg_kh);
    L(kh_label); {
        xor_(b_ic, b_ic);
        L(ic_block_label); {
            if (jcp.l_pad != 0) {
                ur_w_trips--;
                compute_ic_block_step(ur_w, jcp.l_pad, 0, ic_block_step, 0, 0, 0);

                add(reg_input, sizeof(float)*(ur_w*stride_w - jcp.l_pad)*ic_block);
                add(reg_output, sizeof(float)*ur_w*oc_block);
            }

            if (ur_w_trips > 0) {
                xor_(reg_ur_w_trips, reg_ur_w_trips);
                L(ow_block_label); {
                    compute_ic_block_step(ur_w, 0, 0, ic_block_step, 0, 0, 0);
                    add(reg_input, sizeof(float)*ur_w*stride_w*ic_block);
                    add(reg_output, sizeof(float)*ur_w*oc_block);

                    inc(reg_ur_w_trips);
                    cmp(reg_ur_w_trips, ur_w_trips);
                    jl(ow_block_label, T_NEAR);
                }
            }

            if (ur_w_tail > 0) {
                compute_ic_block_step(ur_w_tail, 0, r_pad, ic_block_step, 0, 0,
                        0);
            }

            sub(reg_input, sizeof(float)*input_comeback);
            sub(reg_output, sizeof(float)*output_comeback);

            add(reg_input, sizeof(float)*ic_block_step);
            add(reg_kernel, sizeof(float)*ic_block_step*oc_block);

            add(b_ic, ic_block_step);
            cmp(b_ic, jcp.ic_block);
            jl(ic_block_label, T_NEAR);
        }

        add(reg_input, sizeof(float)*(jcp.iw - 1)*ic_block);
        add(reg_kernel, sizeof(float)*(jcp.kw - 1)*ic_block*oc_block);
        dec(kj);
        cmp(kj, 0);
        jg(kh_label, T_NEAR);
    }
}

inline void jit_avx2_conv_bwd_weights_kernel_f32::compute_oh_loop_common() {
    const int icoc_block = jcp.ic_block*jcp.oc_block;
    const int t_pad = jcp.t_pad;
    const int stride_h = jcp.stride_h;
    int b_pad = nstl::max(0, (jcp.oh - 1)*stride_h + jcp.kh - jcp.ih - t_pad);

    mov(reg_kh, jcp.kh);
    xor_(reg_ih_count, reg_ih_count);
    xor_(reg_oj, reg_oj);
    if (t_pad > 0) {
        mov(reg_kh, jcp.kh - t_pad);
        add(reg_kernel, sizeof(float)*t_pad*jcp.kw*icoc_block);

        L(".oh_tpad_label"); {
            compute_oh_step_disp(".L_kh_top", "L.ic_block_top",
                    "L.ow_block_top", "L.kh_comeback_top");
            add(reg_output, sizeof(float)*jcp.ow*jcp.oc_block);
            sub(reg_kernel, sizeof(float)*stride_h*jcp.kw*icoc_block);

            inc(reg_oj);
            add(reg_ih_count, stride_h);

            add(reg_kh, stride_h);
            cmp(reg_kh, jcp.kh);
            jl(".oh_tpad_label", T_NEAR);
        }

        if (t_pad % stride_h != 0) {
            int inp_corr = stride_h - t_pad % stride_h;
            add(reg_kernel, sizeof(float)*inp_corr*jcp.kw*icoc_block);
            add(reg_input, sizeof(float)*inp_corr*jcp.iw*jcp.ic_block);
        }
    }

    cmp(reg_ih_count, jcp.ih + t_pad - jcp.kh + 1);
    jge(".oh_label_end", T_NEAR);
    cmp(reg_oj, jcp.oh);
    jge(".oh_label", T_NEAR);

    mov(reg_kh, jcp.kh);
    L(".oh_label"); {
        compute_oh_step_disp(".L_kh_center", "L.ic_block_center",
                "L.ow_block_center", "L.kh_comeback_center");
        add(reg_input, sizeof(float)*stride_h*jcp.iw*jcp.ic_block);
        add(reg_output, sizeof(float)*jcp.ow*jcp.oc_block);

        inc(reg_oj);
        add(reg_ih_count, stride_h);

        cmp(reg_ih_count, jcp.ih + t_pad - jcp.kh + 1);
        jge(".oh_label_end", T_NEAR);

        cmp(reg_oj, jcp.oh);
        jl(".oh_label", T_NEAR);
    }
    L(".oh_label_end");

    if (b_pad > 0) {
        cmp(reg_oj, jcp.oh);
        jge(".oh_bpad_label_end", T_NEAR);

        mov(reg_kh, jcp.ih + t_pad);
        sub(reg_kh, reg_ih_count);
        L(".oh_bpad_label"); {
            compute_oh_step_disp(".L_kh_bottom", "L.ic_block_bottom",
                    "L.ow_block_bottom", "L.kh_comeback_bottom");
            add(reg_input, sizeof(float)*stride_h*jcp.iw*jcp.ic_block);
            add(reg_output, sizeof(float)*jcp.ow*jcp.oc_block);

            sub(reg_kh, stride_h);
            cmp(reg_kh, 0);
            jle(".oh_bpad_label_end", T_NEAR);

            inc(reg_oj);
            cmp(reg_oj, jcp.oh);
            jl(".oh_bpad_label", T_NEAR);
        }
        L(".oh_bpad_label_end");
    }
}

}
}
}

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
