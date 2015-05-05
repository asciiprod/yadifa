/*------------------------------------------------------------------------------
*
* Copyright (c) 2011, EURid. All rights reserved.
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


// general defines
#define     PROGRAM_NAME                            PACKAGE
#define     PROGRAM_VERSION                         PACKAGE_VERSION
#define     RELEASEDATE                             "2015-04-24"
#define     COMPILEDATE                             __DATE__

#define     ROUND_ROBIN                             0x00

// yadig
#define     QM_FLAGS_NONE                           0x00
#define     QM_FLAGS_AAONLY                         0x01
#define     QM_FLAGS_AD                             0x02
#define     QM_FLAGS_CD                             0x04
#define     QM_FLAGS_ROUND_ROBIN                    0x08

#define     QM_FLAGS_DNSSEC                         0x10
#define     QM_FLAGS_INGORE_TC                      0x20
#define     QM_FLAGS_RECURSIVE                      0x40
#define     QM_FLAGS_TRACE                          0x80


#define     QM_PROTOCOL_IPV4                        0x01
#define     QM_PROTOCOL_IPV6                        0x02
#define     QM_PROTOCOL_TCP                         0x04
#define     QM_PROTOCOL_UDP                         0x08

#define     VM_DEFAULT                              0x00
#define     VM_JSON                                 0x01
#define     VM_MULTILINE                            0x02
#define     VM_PARSE_FRIENDLY                       0x04
#define     VM_SHORT                                0x05
#define     VM_XML                                  0x06
#define     VM_WIRE                                 0x07

#define     VM_WITH_ADDITIONAL                      0x01
#define     VM_WITH_ANSWER                          0x02
#define     VM_WITH_AUTHORITY                       0x04
#define     VM_WITH_QUESTION                        0x08


#define     DEF_VAL_CONF_OPTION_QM                  "0x00" /* FLAGS_NONE */

#define     DEF_VAL_SERVER_PORT                      53
#define     DEF_VAL_SERVERPORT                      "53"
#define     DEF_VAL_SERVER                          "127.0.0.1 port 53"


