/*
 * SPDX-FileCopyrightText: Copyright 2020, 2022-2023 Arm Limited and/or its affiliates <open-source-office@arm.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>

#ifndef MLW_DECODE_H
#define MLW_DECODE_H

/*
 * Decode an mlw-compressed weight bitstream.
 *
 *   inbuf       compressed bitstream
 *   inbuf_size  size of the compressed bitstream in bytes
 *   outbuf      receives a malloc()ed buffer of 9-bit signed weights
 *   verbose     if non-zero, printf log
 *
 * Returns the number of decoded weights, or 0 on a malformed bitstream
 * (QEMU adaptation: see README.qemu - the upstream code instead exit()s).
 */
int mlw_decode(uint8_t *inbuf, int inbuf_size, int16_t **outbuf, int verbose);

#endif
