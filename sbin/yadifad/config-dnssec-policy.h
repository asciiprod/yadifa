/*------------------------------------------------------------------------------
 *
 * Copyright (c) 2011-2016, EURid. All rights reserved.
 * The YADIFA TM software product is provided under the BSD 3-clause license:
 * 
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *        * Redistributions of source code must retain the above copyright 
 *          notice, this list of conditions and the following disclaimer.
 *        * Redistributions in binary form must reproduce the above copyright 
 *          notice, this list of conditions and the following disclaimer in the 
 *          documentation and/or other materials provided with the distribution.
 *        * Neither the name of EURid nor the names of its contributors may be 
 *          used to endorse or promote products derived from this software 
 *          without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *------------------------------------------------------------------------------
 *
 */

/** @defgroup yadifad
 *  @ingroup configuration
 *  @brief
 */

#pragma once

#include <dnscore/sys_types.h>

ya_result config_register_key_roll(const char *null_or_key_name, s32 priority);
ya_result config_register_key_suite(const char *null_or_key_name, s32 priority);
ya_result config_register_key_template(const char *null_or_key_name, s32 priority);
ya_result config_register_denial(const char *null_or_key_name, s32 priority);


/**
 * @fn ya_result config_register_dnssec_policy(const char *null_or_key_name, s32 priority)
 *
 * @brief register all sections needed for <dnssec-policy> sections
 *
 * @details
 * <key-roll>, <key-template>, <denial> and <key-suite> are needed for <dnssec-policy>
 * get all of them before registering all <dnssec-policy> sections
 *
 * @param[in] const char *null_or_key_name
 * @param[in] s32 priority
 *
 *
 * @retval    return_code -- from other functions
 *
 * return ya_result
 */
ya_result config_register_dnssec_policy(const char *null_or_key_name, s32 priority);

