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
/** @defgroup network Network functions
 *  @ingroup dnscore
 *  @brief
 *
 * @{
 */
/*----------------------------------------------------------------------------*/

#ifndef NETWORK_H
#define NETWORK_H

#include <sys/types.h>	/* Required for BSD */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#include <dnscore/sys_types.h>

#define SOCKADD4_TAG 0x344444414b434f53
#define SOCKADD6_TAG 0x364444414b434f53

/*
 * In order to avoid casting, this is the type that should be used to store sockaddr
 */

typedef union socketaddress socketaddress;

union socketaddress
{
    struct sockaddr         sa;
    struct sockaddr_in      sa4;
    struct sockaddr_in6     sa6;
    struct sockaddr_storage ss;
};

static inline bool sockaddr_equals(struct sockaddr *a, struct sockaddr *b)
{
    if(a->sa_family == b->sa_family)
    {
        switch (a->sa_family)
        {
            case AF_INET:
            {
                struct sockaddr_in *sa4 = (struct sockaddr_in *)a;
                struct sockaddr_in *sb4 = (struct sockaddr_in *)b;

                return memcmp(&sa4->sin_addr.s_addr, &sb4->sin_addr.s_addr, 4) == 0;
            }
            case  AF_INET6:
            {

                struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)a;
                struct sockaddr_in6 *sb6 = (struct sockaddr_in6 *)b;

                return memcmp(&sa6->sin6_addr, &sb6->sin6_addr, 16) == 0;
            }
        }
    }

    return FALSE;
} 


#endif /* HOST_ADDRESS_H */

/** @} */

