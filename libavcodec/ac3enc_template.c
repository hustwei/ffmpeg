/*
 * AC-3 encoder float/fixed template
 * Copyright (c) 2000 Fabrice Bellard
 * Copyright (c) 2006-2011 Justin Ruggles <justin.ruggles@gmail.com>
 * Copyright (c) 2006-2010 Prakash Punnoor <prakash@punnoor.de>
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

/**
 * @file
 * AC-3 encoder float/fixed template
 */

#include <stdint.h>

#include "ac3enc.h"


/**
 * Deinterleave input samples.
 * Channels are reordered from Libav's default order to AC-3 order.
 */
void AC3_NAME(deinterleave_input_samples)(AC3EncodeContext *s,
                                          const SampleType *samples)
{
    int ch, i;

    /* deinterleave and remap input samples */
    for (ch = 0; ch < s->channels; ch++) {
        const SampleType *sptr;
        int sinc;

        /* copy last 256 samples of previous frame to the start of the current frame */
        memcpy(&s->planar_samples[ch][0], &s->planar_samples[ch][AC3_FRAME_SIZE],
               AC3_BLOCK_SIZE * sizeof(s->planar_samples[0][0]));

        /* deinterleave */
        sinc = s->channels;
        sptr = samples + s->channel_map[ch];
        for (i = AC3_BLOCK_SIZE; i < AC3_FRAME_SIZE+AC3_BLOCK_SIZE; i++) {
            s->planar_samples[ch][i] = *sptr;
            sptr += sinc;
        }
    }
}


/**
 * Apply the MDCT to input samples to generate frequency coefficients.
 * This applies the KBD window and normalizes the input to reduce precision
 * loss due to fixed-point calculations.
 */
void AC3_NAME(apply_mdct)(AC3EncodeContext *s)
{
    int blk, ch;

    for (ch = 0; ch < s->channels; ch++) {
        for (blk = 0; blk < AC3_MAX_BLOCKS; blk++) {
            AC3Block *block = &s->blocks[blk];
            const SampleType *input_samples = &s->planar_samples[ch][blk * AC3_BLOCK_SIZE];

            s->apply_window(&s->dsp, s->windowed_samples, input_samples,
                            s->mdct->window, AC3_WINDOW_SIZE);

            if (s->fixed_point)
                block->coeff_shift[ch+1] = s->normalize_samples(s);

            s->mdct->fft.mdct_calcw(&s->mdct->fft, block->mdct_coef[ch+1],
                                    s->windowed_samples);
        }
    }
}


/**
 * Calculate a single coupling coordinate.
 */
static inline float calc_cpl_coord(float energy_ch, float energy_cpl)
{
    float coord = 0.125;
    if (energy_cpl > 0)
        coord *= sqrtf(energy_ch / energy_cpl);
    return coord;
}


/**
 * Calculate coupling channel and coupling coordinates.
 * TODO: Currently this is only used for the floating-point encoder. I was
 *       able to make it work for the fixed-point encoder, but quality was
 *       generally lower in most cases than not using coupling. If a more
 *       adaptive coupling strategy were to be implemented it might be useful
 *       at that time to use coupling for the fixed-point encoder as well.
 */
void AC3_NAME(apply_channel_coupling)(AC3EncodeContext *s)
{
#if CONFIG_AC3ENC_FLOAT
    LOCAL_ALIGNED_16(float,   cpl_coords,       [AC3_MAX_BLOCKS], [AC3_MAX_CHANNELS][16]);
    LOCAL_ALIGNED_16(int32_t, fixed_cpl_coords, [AC3_MAX_BLOCKS], [AC3_MAX_CHANNELS][16]);
    int blk, ch, bnd, i, j;
    CoefSumType energy[AC3_MAX_BLOCKS][AC3_MAX_CHANNELS][16] = {{{0}}};
    int num_cpl_coefs = s->num_cpl_subbands * 12;

    memset(cpl_coords,       0, AC3_MAX_BLOCKS * sizeof(*cpl_coords));
    memset(fixed_cpl_coords, 0, AC3_MAX_BLOCKS * sizeof(*fixed_cpl_coords));

    /* calculate coupling channel from fbw channels */
    for (blk = 0; blk < AC3_MAX_BLOCKS; blk++) {
        AC3Block *block = &s->blocks[blk];
        CoefType *cpl_coef = &block->mdct_coef[CPL_CH][s->start_freq[CPL_CH]];
        if (!block->cpl_in_use)
            continue;
        memset(cpl_coef-1, 0, (num_cpl_coefs+4) * sizeof(*cpl_coef));
        for (ch = 1; ch <= s->fbw_channels; ch++) {
            CoefType *ch_coef = &block->mdct_coef[ch][s->start_freq[CPL_CH]];
            if (!block->channel_in_cpl[ch])
                continue;
            for (i = 0; i < num_cpl_coefs; i++)
                cpl_coef[i] += ch_coef[i];
        }
        /* note: coupling start bin % 4 will always be 1 and num_cpl_coefs
                 will always be a multiple of 12, so we need to subtract 1 from
                 the start and add 4 to the length when using optimized
                 functions which require 16-byte alignment. */

        /* coefficients must be clipped to +/- 1.0 in order to be encoded */
        s->dsp.vector_clipf(cpl_coef-1, cpl_coef-1, -1.0f, 1.0f, num_cpl_coefs+4);

        /* scale coupling coefficients from float to 24-bit fixed-point */
        s->ac3dsp.float_to_fixed24(&block->fixed_coef[CPL_CH][s->start_freq[CPL_CH]-1],
                                   cpl_coef-1, num_cpl_coefs+4);
    }

    /* calculate energy in each band in coupling channel and each fbw channel */
    /* TODO: possibly use SIMD to speed up energy calculation */
    bnd = 0;
    i = s->start_freq[CPL_CH];
    while (i < s->cpl_end_freq) {
        int band_size = s->cpl_band_sizes[bnd];
        for (ch = CPL_CH; ch <= s->fbw_channels; ch++) {
            for (blk = 0; blk < AC3_MAX_BLOCKS; blk++) {
                AC3Block *block = &s->blocks[blk];
                if (!block->cpl_in_use || (ch > CPL_CH && !block->channel_in_cpl[ch]))
                    continue;
                for (j = 0; j < band_size; j++) {
                    CoefType v = block->mdct_coef[ch][i+j];
                    MAC_COEF(energy[blk][ch][bnd], v, v);
                }
            }
        }
        i += band_size;
        bnd++;
    }

    /* determine which blocks to send new coupling coordinates for */
    for (blk = 0; blk < AC3_MAX_BLOCKS; blk++) {
        AC3Block *block  = &s->blocks[blk];
        AC3Block *block0 = blk ? &s->blocks[blk-1] : NULL;
        int new_coords = 0;
        CoefSumType coord_diff[AC3_MAX_CHANNELS] = {0,};

        if (block->cpl_in_use) {
            /* calculate coupling coordinates for all blocks and calculate the
               average difference between coordinates in successive blocks */
            for (ch = 1; ch <= s->fbw_channels; ch++) {
                if (!block->channel_in_cpl[ch])
                    continue;

                for (bnd = 0; bnd < s->num_cpl_bands; bnd++) {
                    cpl_coords[blk][ch][bnd] = calc_cpl_coord(energy[blk][ch][bnd],
                                                              energy[blk][CPL_CH][bnd]);
                    if (blk > 0 && block0->cpl_in_use &&
                        block0->channel_in_cpl[ch]) {
                        coord_diff[ch] += fabs(cpl_coords[blk-1][ch][bnd] -
                                               cpl_coords[blk  ][ch][bnd]);
                    }
                }
                coord_diff[ch] /= s->num_cpl_bands;
            }

            /* send new coordinates if this is the first block, if previous
             * block did not use coupling but this block does, the channels
             * using coupling has changed from the previous block, or the
             * coordinate difference from the last block for any channel is
             * greater than a threshold value. */
            if (blk == 0) {
                new_coords = 1;
            } else if (!block0->cpl_in_use) {
                new_coords = 1;
            } else {
                for (ch = 1; ch <= s->fbw_channels; ch++) {
                    if (block->channel_in_cpl[ch] && !block0->channel_in_cpl[ch]) {
                        new_coords = 1;
                        break;
                    }
                }
                if (!new_coords) {
                    for (ch = 1; ch <= s->fbw_channels; ch++) {
                        if (block->channel_in_cpl[ch] && coord_diff[ch] > 0.04) {
                            new_coords = 1;
                            break;
                        }
                    }
                }
            }
        }
        block->new_cpl_coords = new_coords;
    }

    /* calculate final coupling coordinates, taking into account reusing of
       coordinates in successive blocks */
    for (bnd = 0; bnd < s->num_cpl_bands; bnd++) {
        blk = 0;
        while (blk < AC3_MAX_BLOCKS) {
            int blk1;
            CoefSumType energy_cpl;
            AC3Block *block  = &s->blocks[blk];

            if (!block->cpl_in_use) {
                blk++;
                continue;
            }

            energy_cpl = energy[blk][CPL_CH][bnd];
            blk1 = blk+1;
            while (!s->blocks[blk1].new_cpl_coords && blk1 < AC3_MAX_BLOCKS) {
                if (s->blocks[blk1].cpl_in_use)
                    energy_cpl += energy[blk1][CPL_CH][bnd];
                blk1++;
            }

            for (ch = 1; ch <= s->fbw_channels; ch++) {
                CoefType energy_ch;
                if (!block->channel_in_cpl[ch])
                    continue;
                energy_ch = energy[blk][ch][bnd];
                blk1 = blk+1;
                while (!s->blocks[blk1].new_cpl_coords && blk1 < AC3_MAX_BLOCKS) {
                    if (s->blocks[blk1].cpl_in_use)
                        energy_ch += energy[blk1][ch][bnd];
                    blk1++;
                }
                cpl_coords[blk][ch][bnd] = calc_cpl_coord(energy_ch, energy_cpl);
            }
            blk = blk1;
        }
    }

    /* calculate exponents/mantissas for coupling coordinates */
    for (blk = 0; blk < AC3_MAX_BLOCKS; blk++) {
        AC3Block *block = &s->blocks[blk];
        if (!block->cpl_in_use || !block->new_cpl_coords)
            continue;

        s->ac3dsp.float_to_fixed24(fixed_cpl_coords[blk][1],
                                   cpl_coords[blk][1],
                                   s->fbw_channels * 16);
        s->ac3dsp.extract_exponents(block->cpl_coord_exp[1],
                                    fixed_cpl_coords[blk][1],
                                    s->fbw_channels * 16);

        for (ch = 1; ch <= s->fbw_channels; ch++) {
            int bnd, min_exp, max_exp, master_exp;

            /* determine master exponent */
            min_exp = max_exp = block->cpl_coord_exp[ch][0];
            for (bnd = 1; bnd < s->num_cpl_bands; bnd++) {
                int exp = block->cpl_coord_exp[ch][bnd];
                min_exp = FFMIN(exp, min_exp);
                max_exp = FFMAX(exp, max_exp);
            }
            master_exp = ((max_exp - 15) + 2) / 3;
            master_exp = FFMAX(master_exp, 0);
            while (min_exp < master_exp * 3)
                master_exp--;
            for (bnd = 0; bnd < s->num_cpl_bands; bnd++) {
                block->cpl_coord_exp[ch][bnd] = av_clip(block->cpl_coord_exp[ch][bnd] -
                                                        master_exp * 3, 0, 15);
            }
            block->cpl_master_exp[ch] = master_exp;

            /* quantize mantissas */
            for (bnd = 0; bnd < s->num_cpl_bands; bnd++) {
                int cpl_exp  = block->cpl_coord_exp[ch][bnd];
                int cpl_mant = (fixed_cpl_coords[blk][ch][bnd] << (5 + cpl_exp + master_exp * 3)) >> 24;
                if (cpl_exp == 15)
                    cpl_mant >>= 1;
                else
                    cpl_mant -= 16;

                block->cpl_coord_mant[ch][bnd] = cpl_mant;
            }
        }
    }

    if (CONFIG_EAC3_ENCODER && s->eac3)
        ff_eac3_set_cpl_states(s);
#endif /* CONFIG_AC3ENC_FLOAT */
}


/**
 * Determine rematrixing flags for each block and band.
 */
void AC3_NAME(compute_rematrixing_strategy)(AC3EncodeContext *s)
{
    int nb_coefs;
    int blk, bnd, i;
    AC3Block *block, *av_uninit(block0);

    if (s->channel_mode != AC3_CHMODE_STEREO)
        return;

    for (blk = 0; blk < AC3_MAX_BLOCKS; blk++) {
        block = &s->blocks[blk];
        block->new_rematrixing_strategy = !blk;

        if (!s->rematrixing_enabled) {
            block0 = block;
            continue;
        }

        block->num_rematrixing_bands = 4;
        if (block->cpl_in_use) {
            block->num_rematrixing_bands -= (s->start_freq[CPL_CH] <= 61);
            block->num_rematrixing_bands -= (s->start_freq[CPL_CH] == 37);
            if (blk && block->num_rematrixing_bands != block0->num_rematrixing_bands)
                block->new_rematrixing_strategy = 1;
        }
        nb_coefs = FFMIN(block->end_freq[1], block->end_freq[2]);

        for (bnd = 0; bnd < block->num_rematrixing_bands; bnd++) {
            /* calculate calculate sum of squared coeffs for one band in one block */
            int start = ff_ac3_rematrix_band_tab[bnd];
            int end   = FFMIN(nb_coefs, ff_ac3_rematrix_band_tab[bnd+1]);
            CoefSumType sum[4] = {0,};
            for (i = start; i < end; i++) {
                CoefType lt = block->mdct_coef[1][i];
                CoefType rt = block->mdct_coef[2][i];
                CoefType md = lt + rt;
                CoefType sd = lt - rt;
                MAC_COEF(sum[0], lt, lt);
                MAC_COEF(sum[1], rt, rt);
                MAC_COEF(sum[2], md, md);
                MAC_COEF(sum[3], sd, sd);
            }

            /* compare sums to determine if rematrixing will be used for this band */
            if (FFMIN(sum[2], sum[3]) < FFMIN(sum[0], sum[1]))
                block->rematrixing_flags[bnd] = 1;
            else
                block->rematrixing_flags[bnd] = 0;

            /* determine if new rematrixing flags will be sent */
            if (blk &&
                block->rematrixing_flags[bnd] != block0->rematrixing_flags[bnd]) {
                block->new_rematrixing_strategy = 1;
            }
        }
        block0 = block;
    }
}