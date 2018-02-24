/*------------------------------------------------------------------------------
*
* Copyright (c) 2011-2018, EURid vzw. All rights reserved.
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

/*------------------------------------------------------------------------------
 *
 * Copyright (c) 2011-2018, EURid vzw. All rights reserved.
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
 *
 *      SVN Program:
 *              $URL: https://svn.int.eurid.eu/svn/sysdevel/projects/yadifa/tags/release-2.3.8-public/lib/dnsdb/include/dnsdb/zdb-zone-lock-monitor.h $
 *
 *      Creation Date:
 *              22 September 2016, 11:21
 *
 *      Last Update:
 *              $Date: 2018-02-12 10:54:47 +0100 (Mon, 12 Feb 2018) $
 *              $Revision: 7682 $
 *
 *      Last Change:
 *              $Author: ericdf $
 *
 *----------------------------------------------------------------------------*/

#pragma once

#include "dnsdb/zdb-config-features.h"

#if ZDB_HAS_MUTEX_DEBUG_SUPPORT

#include <dnsdb/zdb_zone.h>

#ifdef __cplusplus
extern "C" {
#endif
    
struct zdb_zone_lock_monitor;

struct zdb_zone_lock_monitor *zdb_zone_lock_monitor_new(const zdb_zone *zone, u8 owner, u8 secondary);
struct zdb_zone_lock_monitor *zdb_zone_lock_monitor_get(const zdb_zone *zone);
bool zdb_zone_lock_monitor_release(struct zdb_zone_lock_monitor *holder);
void zdb_zone_lock_monitor_waits(struct zdb_zone_lock_monitor *holder);
void zdb_zone_lock_monitor_resumes(struct zdb_zone_lock_monitor *holder);
void zdb_zone_lock_monitor_exchanges(struct zdb_zone_lock_monitor *holder);
void zdb_zone_lock_monitor_locks(struct zdb_zone_lock_monitor *holder);    
void zdb_zone_lock_monitor_cancels(struct zdb_zone_lock_monitor *holder);
void zdb_zone_lock_monitor_unlocks(struct zdb_zone_lock_monitor *holder);

void zdb_zone_lock_monitor_log();

#ifdef __cplusplus
}
#endif

#endif
