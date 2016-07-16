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
#ifndef PIPE_STREAM_H_
#define PIPE_STREAM_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <dnscore/input_stream.h>
#include <dnscore/output_stream.h>

/**
 * Creates both output and input stream
 * Writing in the output stream makes it available for the input stream
 * This is not thread-safe.
 * 
 * This is used in the XFR input stream.  The internal processing of the XFR stream
 * writes the available bytes to the output so the reader can get them from the
 * input.
 * 
 * @param output
 * @param input
 */
    

void pipe_stream_init(output_stream *output, input_stream *input, u32 buffer_size);

/**
 * 
 * Number of available bytes in the input stream
 * 
 * @param input
 * @return 
 */

ya_result pipe_stream_read_available(input_stream *input);

/**
 * 
 * Room for bytes in the output stream
 * 
 * @param input
 * @return 
 */

ya_result pipe_stream_write_available(output_stream *input);

#ifdef __cplusplus
}
#endif

#endif /* PIPE_STREAM_H_ */

/*    ------------------------------------------------------------    */

/** @} */

/*----------------------------------------------------------------------------*/

