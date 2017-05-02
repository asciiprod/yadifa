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
/** @defgroup threading Threading, pools, queues, ...
 *  @ingroup dnscore
 *  @brief 
 *
 *  This version of the ring buffer uses the condition-wait mechanism instead of the 3-mutex one.
 *  I'll have to bench both versions but the main incentive is to get rid of complains from helgrind
 * 
 * @{
 *
 *----------------------------------------------------------------------------*/
#pragma once

#include <pthread.h>

#include <dnscore/mutex.h>
#include <dnscore/list-dl.h>

#ifdef	__cplusplus
extern "C" {
#endif

struct threaded_dll_cw
{
    list_dl_s queue;
    list_dl_node_s *pool;
    mutex_t mutex;
    cond_t  cond_read;
    cond_t  cond_write;

    u32             max_size;
};

typedef struct threaded_dll_cw threaded_dll_cw;

#define THREADED_SLL_CW_NULL {{NULL,NULL},{NULL,NULL},0}, NULL, PTHREAD_MUTEX_INITIALIZER,PTHREAD_COND_INITIALIZER,PTHREAD_COND_INITIALIZER,MAX_U32}

void  threaded_dll_cw_init(threaded_dll_cw *queue, int max_size);
void  threaded_dll_cw_finalize(threaded_dll_cw *queue);
void  threaded_dll_cw_enqueue(threaded_dll_cw *queue,void* constant_pointer);
bool  threaded_dll_cw_try_enqueue(threaded_dll_cw *queue,void* constant_pointer);

void* threaded_dll_cw_dequeue(threaded_dll_cw *queue);
void* threaded_dll_cw_try_dequeue(threaded_dll_cw *queue);

void  threaded_dll_cw_wait_empty(threaded_dll_cw *queue);
int   threaded_dll_cw_size(threaded_dll_cw *queue);
int   threaded_dll_cw_room(threaded_dll_cw *queue);

/*
 * The queue will block (write) if bigger than this.
 * Note that if the key is already bigger it will blocked (write) until
 * the content is emptied by the readers.
 */

ya_result threaded_dll_cw_set_maxsize(threaded_dll_cw *queue, int max_size);

#ifdef	__cplusplus
}
#endif

/** @} */

/*----------------------------------------------------------------------------*/

