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
/** @defgroup 
 *  @ingroup 
 *  @brief 
 *
 *  
 *
 * @{
 *
 *----------------------------------------------------------------------------*/

#ifndef __ZDB_ZONE_AXFR_INPUT_STREAM__H__
#define __ZDB_ZONE_AXFR_INPUT_STREAM__H__

#include <dnscore/input_stream.h>
#include <dnsdb/zdb_zone.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opens the specified file as an AXFR image for the given zone.
 * Will fail if the file cannot be opened.
 * 
 * @param is
 * @param zone
 * @param filepath
 * @return 
 */
    
ya_result zdb_zone_axfr_input_stream_open_with_path(input_stream *is, zdb_zone *zone, const char *filepath);
    
/**
 * Opens the file on disk and starts to stream it until it has been marked as fully written
 * Used to send to slaves.
 * 
 * @param is
 * @param zone
 * @return 
 */
    
ya_result zdb_zone_axfr_input_stream_open(input_stream *is, zdb_zone *zone);

#ifdef __cplusplus
}
#endif

#endif /* __ZDB_ZONE_AXFR_INPUT_STREAM__H__ */

/*    ------------------------------------------------------------    */

/** @} */

/*----------------------------------------------------------------------------*/

