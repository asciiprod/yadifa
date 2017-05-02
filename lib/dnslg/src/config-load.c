/*------------------------------------------------------------------------------
 *
 * Copyright (c) 2011-2017, EURid. All rights reserved.
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
#include <sys/stat.h>

#include <dnscore/sys_types.h>
#include <dnscore/input_stream.h>
#include <dnscore/config_settings.h>
#include <dnscore/bytearray_input_stream.h>
#include <dnslg/resolv-conf.h>

ya_result
config_load_rc(const char *file_path)
{
    config_error_s                                                   cfgerr;
    ya_result                                                   return_code;

    /*    ------------------------------------------------------------    */


    // check if file exist then parse it
    struct stat s;
    if((return_code = stat(file_path, &s)) >= 0)
    {
        config_set_source(CONFIG_SOURCE_FILE);

        if(FAIL(return_code = config_read(file_path, &cfgerr)))
        {
            formatln("%s: parsing error: %s:%u : '%s': %r", "resolver", cfgerr.file, cfgerr.line_number, cfgerr.line, return_code);
            flushout();

            return return_code;
        }

        return 1;
    }

    return OK;
}


ya_result
config_load_resolv_conf(void)
{
    input_stream                                                  config_is;
    config_error_s                                                   cfgerr;
    ya_result                                                   return_code;

    /*    ------------------------------------------------------------    */


    config_set_source(CONFIG_SOURCE_FILE - 1);

    resolv_conf_parse(&config_is);

    if(FAIL(return_code = config_read_from_buffer((const char*)bytearray_input_stream_buffer(&config_is),
                    bytearray_input_stream_size(&config_is), "/etc/resolv.conf",
                    &cfgerr)))
    {
        formatln("%s: parsing error: %s:%u : '%s': %r", "resolver", cfgerr.file, cfgerr.line_number, cfgerr.line, return_code);
        flushout();

        input_stream_close(&config_is);

        return return_code;
    }

    input_stream_close(&config_is);

    return OK;
}
