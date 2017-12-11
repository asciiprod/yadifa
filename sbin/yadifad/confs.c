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
/** @defgroup config Configuration handling
 *  @ingroup yadifad
 *  @brief
 *
 * @{
 */
/*------------------------------------------------------------------------------
 *
 * USE INCLUDES */

#include "server-config.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <getopt.h>
#include <pwd.h>
#include <grp.h>

#include "config.h"

#if HAS_DNSSEC_SUPPORT != 0
#include <dnsdb/dnssec.h>
#include <dnsdb/dnssec-keystore.h>
#endif

#include <dnscore/base64.h>
#include <dnscore/format.h>
#include <dnscore/logger.h>
#include <dnscore/message.h>
#include <dnscore/sys_get_cpu_count.h>
#include <dnscore/parsing.h>
#include <dnscore/cmdline.h>
#include <dnscore/file_output_stream.h>
#include <dnscore/logger_channel_stream.h>
#include <dnscore/config-cmdline.h>
#include <dnscore/tsig.h>
#include <dnscore/fdtools.h>

#if HAS_DNSSEC_SUPPORT
#include <dnsdb/dnssec.h>
#endif

#define ZDB_JOURNAL_CODE 1
#include <dnsdb/journal.h>

#include "buildinfo.h"

extern logger_handle *g_server_logger;
#define MODULE_MSG_HANDLE g_server_logger

#include "zone.h"
#include "config.h"
#include "server.h"
#include "confs.h"
#include "database-service.h"

#include "server_error.h"
#include "config_error.h"

#include "config_acl.h"

#define CONFSDSP_TAG 0x50534453464e4f43
#define CONFSPL_TAG 0x4c5053464e4f43

/*
 * 2011/10/18 : EDF: disabling the debug because it makes the legitimate error output unreadable.
 */

#undef DEBUGLNF
#undef DEBUGF
#undef OSDEBUG
#undef LDEBUG
#undef OSLDEBUG
#define DEBUGLNF(...)
#define DEBUGF(...)
#define OSDEBUG(...)
#define LDEBUG(...)
#define OSLDEBUG(...)

#ifndef NAME_MAX
#define NAME_MAX 1024
#endif

struct logger_name_handle_s
{
    const char *name;
    logger_handle **handlep;
};

extern logger_handle* g_system_logger;
extern logger_handle* g_database_logger;
extern logger_handle* g_dnssec_logger;
extern logger_handle* g_zone_logger;
extern logger_handle* g_server_logger;
extern logger_handle* g_statistics_logger;
extern logger_handle* g_queries_logger;

static const struct logger_name_handle_s logger_name_handles[] =
{
    {"system", &g_system_logger},
    {"database", &g_database_logger},
#if HAS_DNSSEC_SUPPORT != 0
    {"dnssec", &g_dnssec_logger},
#endif
    {"zone", &g_zone_logger},
    {"server", &g_server_logger},
    {"stats", &g_statistics_logger},
    {"queries", &g_queries_logger},
    {NULL, NULL}
};

CMDLINE_BEGIN(yadifad_cmdline)
CMDLINE_SECTION("main")
CMDLINE_OPT("config",'c',"config_file")
CMDLINE_BOOL("daemon", 'd', "daemon")
CMDLINE_BOOL_NOT("nodaemon", 0, "daemon")
CMDLINE_BOOL("log", 'L', "log_from_start")

CMDLINE_OPT("uid", 'u', "uid")
CMDLINE_OPT("gid", 'g', "gid")
CMDLINE_OPT("port", 'P', "server_port")
CMDLINE_VERSION_HELP(yadifad_cmdline)
CMDLINE_END(yadifad_cmdline)

static const char *default_channel = "stdout default";

void
config_logger_setdefault()
{
    logger_start();
    
    output_stream stdout_os;
    logger_channel *stdout_channel;

    fd_output_stream_attach(&stdout_os, dup_ex(1));
    stdout_channel = logger_channel_alloc();
    logger_channel_stream_open(&stdout_os, FALSE, stdout_channel);
    logger_channel_register(default_channel, stdout_channel);

    for(const struct logger_name_handle_s *name_handle = logger_name_handles; name_handle->name != NULL; name_handle++)
    {
        logger_handle_create(name_handle->name, name_handle->handlep);
#ifndef DEBUG
        logger_handle_add_channel(name_handle->name, MSG_PROD_MASK, default_channel);
#else
        logger_handle_add_channel(name_handle->name, MSG_ALL_MASK, default_channel);
#endif
    }

#ifdef DEBUG
    log_debug("logging to stdout");
#endif
}

void
config_logger_cleardefault()
{
    for(const struct logger_name_handle_s *name_handle = logger_name_handles; name_handle->name != NULL; name_handle++)
    {
        logger_handle_remove_channel(name_handle->name, default_channel);
    }
    
    logger_channel_unregister(default_channel);
}

static void
yadifad_print_usage()
{
    print("\n"
          "\t\toptions:\n"
          "\t\t--config/-c <config_file>   : load the configuration from <config_file>\n"
          "\t\t--daemon/-d                 : overrides the daemon setting, enables it\n"
          "\t\t--nodaemon                  : overrides the daemon setting, disables it\n"
          "\t\t--uid/-u userid             : overrides the uid setting\n"
          "\t\t--gid/-g groupid            : overrides the gid setting\n"
          "\t\t--port/-P port              : overrides the server-port setting\n"

          "\n"

          "\n"
          "\t\t--version/-V                : view version\n"
          "\t\t--help/-h                   : show this help text\n"
        );
    flushout();
}

static void
yadifad_print_authors()
{
    print("\n"
          "\t\tYADIFAD authors:\n"
          "\t\t---------------\n"
          "\t\t\n"
          "\t\tGery Van Emelen\n"
          "\t\tEric Diaz Fernandez\n"
          "\n"
          "\t\tContact: " PACKAGE_BUGREPORT "\n"
         );
    flushout();
}

static void
yadifad_show_version(u8 level)
{
    switch(level)
    {
	case 1:
	    osformatln(termout, "%s %s (%s)\n", PROGRAM_NAME, PROGRAM_VERSION, RELEASEDATE);
	    break;
	case 2:
#if HAS_BUILD_TIMESTAMP && defined(__DATE__)
	    osformatln(termout, "%s %s (released %s, compiled %s)\n\nbuild settings: %s\n", PROGRAM_NAME, PROGRAM_VERSION, RELEASEDATE, __DATE__, BUILD_OPTIONS);
#else
            osformatln(termout, "%s %s (released %s)\n\nbuild settings: %s\n", PROGRAM_NAME, PROGRAM_VERSION, RELEASEDATE, BUILD_OPTIONS);
#endif
	    break;
        case 3:
#if HAS_BUILD_TIMESTAMP && defined(__DATE__)
	    osformatln(termout, "%s %s (released %s, compiled %s)\n", PROGRAM_NAME, PROGRAM_VERSION, RELEASEDATE, __DATE__);
#else
            osformatln(termout, "%s %s (released %s)\n", PROGRAM_NAME, PROGRAM_VERSION, RELEASEDATE);
#endif
            yadifad_print_authors();
            break;
	default:
	    osformat(termout, "\nYou want to know too much!\n\n");
	    break;
    }
    
    flushout();
}

/** @brief Initialize the config file with the standard settings
 *
 *  @param[out] config
 *
 *  @retval OK
 */

ya_result config_register_main(s32 priority);
#if HAS_ACL_SUPPORT
ya_result config_register_acl(const char *null_or_acl_name, s32 priority);
#endif
ya_result config_register_zone(const char *null_or_key_name, s32 priority);
#if HAS_CTRL
ya_result config_register_control(s32 priority);
#endif
#if HAS_RRL_SUPPORT
ya_result config_register_rrl(s32 priority);
#endif
#if DNSCORE_HAS_NSID_SUPPORT
ya_result config_register_nsid(s32 priority);
#endif
ya_result config_register_dnssec_policy(const char *null_or_key_name, s32 priority);

ya_result
yadifad_config_init()
{
    ya_result return_code;
    
    for(const struct logger_name_handle_s *name_handle = logger_name_handles; name_handle->name != NULL; name_handle++)
    {
        logger_handle_create(name_handle->name, name_handle->handlep);
    }
    
    if(FAIL(return_code = config_init()))
    {
        return return_code;
    }
    
    // to handle version & help
    
    int priority = 0;
    
    if(FAIL(return_code = config_register_cmdline(priority++)))
    {
        return return_code;
    }
            
#if HAS_TSIG_SUPPORT
    if(FAIL(return_code = config_register_key(NULL, priority++)))
    {
        return return_code;
    }
#endif
    
#if HAS_ACL_SUPPORT
    if(FAIL(return_code = config_register_acl(NULL, priority++)))
    {
        return return_code;
    }
#endif

#if HAS_MASTER_SUPPORT && HAS_RRSIG_MANAGEMENT_SUPPORT && HAS_DNSSEC_SUPPORT    
    if(FAIL(return_code = config_register_dnssec_policy(NULL, priority)))
    {
        return return_code;
    }
#endif
    
    priority += 5;
    
    if(FAIL(return_code = config_register_main(priority++)))
    {
        return return_code;
    }
    
    if(FAIL(return_code = config_register_logger(NULL, NULL,priority))) // 5 & 6
    {
        return return_code;
    }
    
    priority += 2;
    
#if HAS_CTRL
    if(FAIL(return_code = config_register_control(priority++)))
    {
        return return_code;
    }
#endif
    
    if(FAIL(return_code = config_register_zone(NULL, priority++)))
    {
        return return_code;
    }

#if HAS_RRL_SUPPORT
    if(FAIL(return_code = config_register_rrl(priority++)))
    {
        return return_code;
    }
#endif
    
#if DNSCORE_HAS_NSID_SUPPORT
    if(FAIL(return_code = config_register_nsid(priority++)))
    {
        return return_code;
    }
#endif

     
    return return_code;
}

static ya_result
yadifad_config_cmdline_callback(const struct cmdline_desc_s *desc, const char *arg_name, void *callback_owned)
{
    if(strcmp(arg_name, "--") == 0)
    {
        return CMDLINE_ARG_STOP_PROCESSING_FLAG_OPTIONS;
    }
    
    return SUCCESS;
}

/**
 * There is an issue using buffer as a an input because there is no rewind feature (yet) 
 * So at the second pass, the reader fails with a bogus error message.
 * 
 * affected: ./sbin/yadifad/yadifad -d
 * outputs: cmdline: parsing error: command-line: 3: '</main>': No such file or directory
 * 
 * @param argc
 * @param argv
 * @return 
 */

ya_result
yadifad_config_cmdline(int argc, char **argv)
{
    input_stream config_is;
    config_error_s cfgerr;
    ya_result return_code;
    
    config_set_source(CONFIG_SOURCE_HIGHEST);
    
    if(FAIL(return_code = cmdline_parse(yadifad_cmdline, argc, argv, yadifad_config_cmdline_callback, NULL, &config_is)))
    {
        formatln("command line: %r", return_code);
        flushout();
        
        return return_code;
    }
    
    config_set_source(CONFIG_SOURCE_CMDLINE);
        
    u32 cmdline_buffer_size = bytearray_input_stream_size(&config_is);
    u8* cmdline_buffer = bytearray_input_stream_detach(&config_is);
    
    input_stream_close(&config_is);

    if(FAIL(return_code = config_read_from_buffer((const char*)cmdline_buffer, cmdline_buffer_size, "command-line", &cfgerr)))
    {
        if(cfgerr.file[0] != '\0')
        {
            formatln("command line: '%s': %r", cfgerr.line, return_code);
            flushout();
        }
        
        free(cmdline_buffer);
        
        return return_code;
    }
    
    free(cmdline_buffer);
    
    return_code = 0;
    
    if(cmdline_version_get() > 0)
    {
        yadifad_show_version(cmdline_version_get());
        
        return_code++;
    }
    
    if(cmdline_help_get())
    {
        yadifad_print_usage();
        return_code++;
    }
    
    config_set_source(CONFIG_SOURCE_DEFAULT);
    
    if(return_code == 0)
    {
        if(ISOK(return_code = config_value_set_to_default("main", "config_file", &cfgerr)))
        {
            return_code = 0;
        }
        else
        {
            if(cfgerr.file[0] != '\0')
            {
                formatln("%s: %r",  cfgerr.file, return_code);
                flushout();
            }
            else
            {
                // should never happen
                
                formatln("error: %r",  cfgerr.file, return_code);
                flushout();
            }
        }
    }

    return return_code;
}

ya_result
yadifad_config_read(const char *config_file)
{
    config_error_s cfgerr;
    ya_result return_code = SUCCESS;
    
    char configuration_file_path[PATH_MAX];
    
    // if the passed value is a pointer into a configuration structure,
    // there is a risk that the value is freed and replaced by a different one
    // => bad
    // so a copy is done first
    
    if(database_zone_try_reconfigure_enable())
    {
        strncpy(configuration_file_path, config_file, sizeof(configuration_file_path));

        struct config_source_s sources[1];
        config_source_set_file(&sources[0], configuration_file_path, CONFIG_SOURCE_FILE);

        return_code =  config_read_from_sources(sources, 1, &cfgerr);

        if(FAIL(return_code))
        {
            if(cfgerr.file[0] != '\0')
            {
                formatln("%s: parsing error: %s: %u: '%s': %r", config_file, cfgerr.file, cfgerr.line_number, cfgerr.line, return_code);
            }
        }

        database_zone_reconfigure_disable();
    }
    else
    {
        formatln("could not read configuration: already reading a configuration");
    }
    
    return return_code;
}

ya_result
yadifad_config_finalise()
{
    ya_result return_code = SUCCESS;
    
    config_set_source(CONFIG_SOURCE_DEFAULT);

    // disable loggers without any channel output
    
    for(const struct logger_name_handle_s *name_handle = logger_name_handles; name_handle->name != NULL; name_handle++)
    {
        if(logger_handle_count_channels(name_handle->name) == 0)
        {
            logger_handle_close(name_handle->name);
        }
    }

    return return_code;
}

ya_result
config_read_zones()
{
    return FEATURE_NOT_IMPLEMENTED_ERROR; // not implemented
}

ya_result
yadifad_config_update(const char *config_file)
{
    if(dnscore_shuttingdown())
    {
        log_try_debug("yadifad_config_update(%s) cancelled by shutdown", config_file);
        return STOPPED_BY_APPLICATION_SHUTDOWN;
    }
    
    log_try_debug("yadifad_config_update(%s) started", config_file);
    
    config_error_s cfgerr;
    ya_result return_code = ERROR;
    
    if(database_zone_try_reconfigure_enable())
    {
        journal_close_unused();
        
        database_set_drop_after_reload_for_set(NULL);

        config_set_source(CONFIG_SOURCE_FILE);
        
#if HAS_TSIG_SUPPORT
        tsig_serial_next();
#endif
        if(ISOK(return_code =  config_read_section(config_file, &cfgerr, "key")))
        {
            if(ISOK(return_code =  config_read_section(config_file, &cfgerr, "zone")))
            {                
                log_info("%s: key and zone sections read", config_file);
            }
            else
            {
                if(cfgerr.file[0] != '\0')
                {
                    ttylog_err("%s: parsing error: %s: %u: '%s': %r", config_file, cfgerr.file, cfgerr.line_number, cfgerr.line, return_code);
                }
                else
                {
                    ttylog_err("%r", return_code);
                }
            }
        }
        else
        {
            if(cfgerr.file[0] != '\0')
            {
                ttylog_err("%s: parsing error: %s: %u: '%s': %r", config_file, cfgerr.file, cfgerr.line_number, cfgerr.line, return_code);
            }
            else
            {
                ttylog_err("%r", return_code);
            }
        }
        
        database_zone_reconfigure_do_drop_and_disable(ISOK(return_code));
        
#if DNSCORE_HAS_DNSSEC_SUPPORT
        dnssec_keystore_reload();
#endif
    }
    else
    {
        log_try_debug("previous reconfigure still running, postponed to run right after");
        database_zone_postpone_reconfigure_all();
    }
    
    log_try_debug("yadifad_config_update(%s): %r", config_file, return_code);
    
    return return_code;
}

static ya_result
yadifad_config_update_zone_filter(zone_desc_s *zone_desc, void *params)
{
    ptr_set *fqdn_set = (ptr_set*)params;
    
    if((fqdn_set == NULL) || (ptr_set_avl_find(fqdn_set, zone_desc->origin) != NULL))
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

ya_result
yadifad_config_update_zone(const char *config_file, const ptr_set *fqdn_set)
{
    if(fqdn_set != NULL)
    {
        log_debug("yadifad_config_update_zone(%s, <set>) started", config_file);
    }
    else
    {
        log_debug("yadifad_config_update_zone(%s, ALL) started", config_file);
    }
    
    config_error_s cfgerr;
    ya_result return_code = ERROR;

    if(database_zone_try_reconfigure_enable())
    {
        database_set_drop_after_reload_for_set(fqdn_set);

        config_set_source(CONFIG_SOURCE_FILE);
        
#if HAS_TSIG_SUPPORT
        tsig_serial_next();
#endif
        
        if(ISOK(return_code = config_read_section(config_file, &cfgerr, "key")))
        {
            config_section_zone_set_filter(yadifad_config_update_zone_filter, (void*)fqdn_set); // the filter will not modify the fqdn
            
            return_code =  config_read_section(config_file, &cfgerr, "zone");
            
            config_section_zone_set_filter(NULL, NULL);
            
            if(ISOK(return_code))
            {
                log_info("%s: key and a some zone sections read", config_file);
            }
            else
            {
                if(cfgerr.file[0] != '\0')
                {
                    ttylog_err("%s: parsing error: %s: %u: '%s': %r", config_file, cfgerr.file, cfgerr.line_number, cfgerr.line, return_code);
                }
                else
                {
                    ttylog_err("%r", return_code);
                }
            }
        }
        else
        {
            if(cfgerr.file[0] != '\0')
            {
                ttylog_err("%s: parsing error: %s: %u: '%s': %r", config_file, cfgerr.file, cfgerr.line_number, cfgerr.line, return_code);
            }
            else
            {
                ttylog_err("%r", return_code);
            }
        }
        
        database_zone_reconfigure_do_drop_and_disable(ISOK(return_code));
    }
    else
    {
        log_debug("previous reconfigure still running, postponed to run right after");
        
        if(fqdn_set != NULL)
        {
            database_zone_postpone_reconfigure_zone(fqdn_set);
        }
        else
        {
            database_zone_postpone_reconfigure_zones();
        }
    }
    
    log_debug("yadifad_config_update_zone(%s, ...): %r", config_file, return_code);
    
    return return_code;
}

/** @} */

/*----------------------------------------------------------------------------*/
