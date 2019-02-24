/*------------------------------------------------------------------------------
*
* Copyright (c) 2011-2019, EURid vzw. All rights reserved.
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
#pragma once

/** @defgroup yadifa
 *  @ingroup ###
 *  @brief yadifa
 */

#include <dnscore/sys_error.h>


typedef struct config_main_settings_s config_main_settings_s;

struct config_main_settings_s
{
    host_address                                                    *server;
    u8                                                               *qname;
    u8                                                       *tsig_key_name;
//    u8                                                                *file;
    char                                                       *config_file;

    u8                                                            log_level;

    /*    ------------------------------------------------------------    */

    int16_t                                                          qclass;
    int16_t                                                           qtype;

    bool                                                              clean;

    /** @todo 20150219 gve -- #if HAS_TCL must be set, before release */
//#if HAS_TCL
    bool                                                        interactive;
//#endif // HAS_TCL
    bool                                                            verbose;
    bool                                                             enable;

};


/*----------------------------------------------------------------------------*/
#pragma mark PROTOTYPES 

ya_result yadifa_config_init();
ya_result yadifa_config_cmdline(int argc, char **argv);
ya_result yadifa_config_finalise();
char *yadifa_config_file_get();
void yadifa_print_usage(void);
bool yadifa_is_interactive(void);

void yadifa_print_version(int level);

/*    ------------------------------------------------------------    */

