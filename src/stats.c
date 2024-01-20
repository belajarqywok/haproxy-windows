/*
 * Functions dedicated to statistics output and the stats socket
 *
 * Copyright 2000-2012 Willy Tarreau <w@1wt.eu>
 * Copyright 2007-2009 Krzysztof Piotr Oledzki <ole@ans.pl>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <grp.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <haproxy/api.h>
#include <haproxy/activity.h>
#include <haproxy/applet.h>
#include <haproxy/backend.h>
#include <haproxy/base64.h>
#include <haproxy/cfgparse.h>
#include <haproxy/channel.h>
#include <haproxy/check.h>
#include <haproxy/cli.h>
#include <haproxy/clock.h>
#include <haproxy/compression.h>
#include <haproxy/debug.h>
#include <haproxy/errors.h>
#include <haproxy/fd.h>
#include <haproxy/freq_ctr.h>
#include <haproxy/frontend.h>
#include <haproxy/global.h>
#include <haproxy/http.h>
#include <haproxy/http_ana.h>
#include <haproxy/http_htx.h>
#include <haproxy/htx.h>
#include <haproxy/list.h>
#include <haproxy/listener.h>
#include <haproxy/log.h>
#include <haproxy/map-t.h>
#include <haproxy/pattern-t.h>
#include <haproxy/pipe.h>
#include <haproxy/pool.h>
#include <haproxy/proxy.h>
#include <haproxy/resolvers.h>
#include <haproxy/sc_strm.h>
#include <haproxy/server.h>
#include <haproxy/session.h>
#include <haproxy/stats.h>
#include <haproxy/stconn.h>
#include <haproxy/stream.h>
#include <haproxy/task.h>
#include <haproxy/ticks.h>
#include <haproxy/time.h>
#include <haproxy/tools.h>
#include <haproxy/uri_auth-t.h>
#include <haproxy/version.h>


/* status codes available for the stats admin page (strictly 4 chars length) */
const char *stat_status_codes[STAT_STATUS_SIZE] = {
	[STAT_STATUS_DENY] = "DENY",
	[STAT_STATUS_DONE] = "DONE",
	[STAT_STATUS_ERRP] = "ERRP",
	[STAT_STATUS_EXCD] = "EXCD",
	[STAT_STATUS_NONE] = "NONE",
	[STAT_STATUS_PART] = "PART",
	[STAT_STATUS_UNKN] = "UNKN",
	[STAT_STATUS_IVAL] = "IVAL",
};

/* These are the field names for each INF_* field position. Please pay attention
 * to always use the exact same name except that the strings for new names must
 * be lower case or CamelCase while the enum entries must be upper case.
 */
const struct name_desc info_fields[INF_TOTAL_FIELDS] = {
	[INF_NAME]                           = { .name = "Name",                        .desc = "Product name" },
	[INF_VERSION]                        = { .name = "Version",                     .desc = "Product version" },
	[INF_RELEASE_DATE]                   = { .name = "Release_date",                .desc = "Date of latest source code update" },
	[INF_NBTHREAD]                       = { .name = "Nbthread",                    .desc = "Number of started threads (global.nbthread)" },
	[INF_NBPROC]                         = { .name = "Nbproc",                      .desc = "Number of started worker processes (historical, always 1)" },
	[INF_PROCESS_NUM]                    = { .name = "Process_num",                 .desc = "Relative worker process number (1)" },
	[INF_PID]                            = { .name = "Pid",                         .desc = "This worker process identifier for the system" },
	[INF_UPTIME]                         = { .name = "Uptime",                      .desc = "How long ago this worker process was started (days+hours+minutes+seconds)" },
	[INF_UPTIME_SEC]                     = { .name = "Uptime_sec",                  .desc = "How long ago this worker process was started (seconds)" },
	[INF_START_TIME_SEC]                 = { .name = "Start_time_sec",              .desc = "Start time in seconds" },
	[INF_MEMMAX_MB]                      = { .name = "Memmax_MB",                   .desc = "Worker process's hard limit on memory usage in MB (-m on command line)" },
	[INF_MEMMAX_BYTES]                   = { .name = "Memmax_bytes",                .desc = "Worker process's hard limit on memory usage in byes (-m on command line)" },
	[INF_POOL_ALLOC_MB]                  = { .name = "PoolAlloc_MB",                .desc = "Amount of memory allocated in pools (in MB)" },
	[INF_POOL_ALLOC_BYTES]               = { .name = "PoolAlloc_bytes",             .desc = "Amount of memory allocated in pools (in bytes)" },
	[INF_POOL_USED_MB]                   = { .name = "PoolUsed_MB",                 .desc = "Amount of pool memory currently used (in MB)" },
	[INF_POOL_USED_BYTES]                = { .name = "PoolUsed_bytes",              .desc = "Amount of pool memory currently used (in bytes)" },
	[INF_POOL_FAILED]                    = { .name = "PoolFailed",                  .desc = "Number of failed pool allocations since this worker was started" },
	[INF_ULIMIT_N]                       = { .name = "Ulimit-n",                    .desc = "Hard limit on the number of per-process file descriptors" },
	[INF_MAXSOCK]                        = { .name = "Maxsock",                     .desc = "Hard limit on the number of per-process sockets" },
	[INF_MAXCONN]                        = { .name = "Maxconn",                     .desc = "Hard limit on the number of per-process connections (configured or imposed by Ulimit-n)" },
	[INF_HARD_MAXCONN]                   = { .name = "Hard_maxconn",                .desc = "Hard limit on the number of per-process connections (imposed by Memmax_MB or Ulimit-n)" },
	[INF_CURR_CONN]                      = { .name = "CurrConns",                   .desc = "Current number of connections on this worker process" },
	[INF_CUM_CONN]                       = { .name = "CumConns",                    .desc = "Total number of connections on this worker process since started" },
	[INF_CUM_REQ]                        = { .name = "CumReq",                      .desc = "Total number of requests on this worker process since started" },
	[INF_MAX_SSL_CONNS]                  = { .name = "MaxSslConns",                 .desc = "Hard limit on the number of per-process SSL endpoints (front+back), 0=unlimited" },
	[INF_CURR_SSL_CONNS]                 = { .name = "CurrSslConns",                .desc = "Current number of SSL endpoints on this worker process (front+back)" },
	[INF_CUM_SSL_CONNS]                  = { .name = "CumSslConns",                 .desc = "Total number of SSL endpoints on this worker process since started (front+back)" },
	[INF_MAXPIPES]                       = { .name = "Maxpipes",                    .desc = "Hard limit on the number of pipes for splicing, 0=unlimited" },
	[INF_PIPES_USED]                     = { .name = "PipesUsed",                   .desc = "Current number of pipes in use in this worker process" },
	[INF_PIPES_FREE]                     = { .name = "PipesFree",                   .desc = "Current number of allocated and available pipes in this worker process" },
	[INF_CONN_RATE]                      = { .name = "ConnRate",                    .desc = "Number of front connections created on this worker process over the last second" },
	[INF_CONN_RATE_LIMIT]                = { .name = "ConnRateLimit",               .desc = "Hard limit for ConnRate (global.maxconnrate)" },
	[INF_MAX_CONN_RATE]                  = { .name = "MaxConnRate",                 .desc = "Highest ConnRate reached on this worker process since started (in connections per second)" },
	[INF_SESS_RATE]                      = { .name = "SessRate",                    .desc = "Number of sessions created on this worker process over the last second" },
	[INF_SESS_RATE_LIMIT]                = { .name = "SessRateLimit",               .desc = "Hard limit for SessRate (global.maxsessrate)" },
	[INF_MAX_SESS_RATE]                  = { .name = "MaxSessRate",                 .desc = "Highest SessRate reached on this worker process since started (in sessions per second)" },
	[INF_SSL_RATE]                       = { .name = "SslRate",                     .desc = "Number of SSL connections created on this worker process over the last second" },
	[INF_SSL_RATE_LIMIT]                 = { .name = "SslRateLimit",                .desc = "Hard limit for SslRate (global.maxsslrate)" },
	[INF_MAX_SSL_RATE]                   = { .name = "MaxSslRate",                  .desc = "Highest SslRate reached on this worker process since started (in connections per second)" },
	[INF_SSL_FRONTEND_KEY_RATE]          = { .name = "SslFrontendKeyRate",          .desc = "Number of SSL keys created on frontends in this worker process over the last second" },
	[INF_SSL_FRONTEND_MAX_KEY_RATE]      = { .name = "SslFrontendMaxKeyRate",       .desc = "Highest SslFrontendKeyRate reached on this worker process since started (in SSL keys per second)" },
	[INF_SSL_FRONTEND_SESSION_REUSE_PCT] = { .name = "SslFrontendSessionReuse_pct", .desc = "Percent of frontend SSL connections which did not require a new key" },
	[INF_SSL_BACKEND_KEY_RATE]           = { .name = "SslBackendKeyRate",           .desc = "Number of SSL keys created on backends in this worker process over the last second" },
	[INF_SSL_BACKEND_MAX_KEY_RATE]       = { .name = "SslBackendMaxKeyRate",        .desc = "Highest SslBackendKeyRate reached on this worker process since started (in SSL keys per second)" },
	[INF_SSL_CACHE_LOOKUPS]              = { .name = "SslCacheLookups",             .desc = "Total number of SSL session ID lookups in the SSL session cache on this worker since started" },
	[INF_SSL_CACHE_MISSES]               = { .name = "SslCacheMisses",              .desc = "Total number of SSL session ID lookups that didn't find a session in the SSL session cache on this worker since started" },
	[INF_COMPRESS_BPS_IN]                = { .name = "CompressBpsIn",               .desc = "Number of bytes submitted to the HTTP compressor in this worker process over the last second" },
	[INF_COMPRESS_BPS_OUT]               = { .name = "CompressBpsOut",              .desc = "Number of bytes emitted by the HTTP compressor in this worker process over the last second" },
	[INF_COMPRESS_BPS_RATE_LIM]          = { .name = "CompressBpsRateLim",          .desc = "Limit of CompressBpsOut beyond which HTTP compression is automatically disabled" },
	[INF_ZLIB_MEM_USAGE]                 = { .name = "ZlibMemUsage",                .desc = "Amount of memory currently used by HTTP compression on the current worker process (in bytes)" },
	[INF_MAX_ZLIB_MEM_USAGE]             = { .name = "MaxZlibMemUsage",             .desc = "Limit on the amount of memory used by HTTP compression above which it is automatically disabled (in bytes, see global.maxzlibmem)" },
	[INF_TASKS]                          = { .name = "Tasks",                       .desc = "Total number of tasks in the current worker process (active + sleeping)" },
	[INF_RUN_QUEUE]                      = { .name = "Run_queue",                   .desc = "Total number of active tasks+tasklets in the current worker process" },
	[INF_IDLE_PCT]                       = { .name = "Idle_pct",                    .desc = "Percentage of last second spent waiting in the current worker thread" },
	[INF_NODE]                           = { .name = "node",                        .desc = "Node name (global.node)" },
	[INF_DESCRIPTION]                    = { .name = "description",                 .desc = "Node description (global.description)" },
	[INF_STOPPING]                       = { .name = "Stopping",                    .desc = "1 if the worker process is currently stopping, otherwise zero" },
	[INF_JOBS]                           = { .name = "Jobs",                        .desc = "Current number of active jobs on the current worker process (frontend connections, master connections, listeners)" },
	[INF_UNSTOPPABLE_JOBS]               = { .name = "Unstoppable Jobs",            .desc = "Current number of unstoppable jobs on the current worker process (master connections)" },
	[INF_LISTENERS]                      = { .name = "Listeners",                   .desc = "Current number of active listeners on the current worker process" },
	[INF_ACTIVE_PEERS]                   = { .name = "ActivePeers",                 .desc = "Current number of verified active peers connections on the current worker process" },
	[INF_CONNECTED_PEERS]                = { .name = "ConnectedPeers",              .desc = "Current number of peers having passed the connection step on the current worker process" },
	[INF_DROPPED_LOGS]                   = { .name = "DroppedLogs",                 .desc = "Total number of dropped logs for current worker process since started" },
	[INF_BUSY_POLLING]                   = { .name = "BusyPolling",                 .desc = "1 if busy-polling is currently in use on the worker process, otherwise zero (config.busy-polling)" },
	[INF_FAILED_RESOLUTIONS]             = { .name = "FailedResolutions",           .desc = "Total number of failed DNS resolutions in current worker process since started" },
	[INF_TOTAL_BYTES_OUT]                = { .name = "TotalBytesOut",               .desc = "Total number of bytes emitted by current worker process since started" },
	[INF_TOTAL_SPLICED_BYTES_OUT]        = { .name = "TotalSplicedBytesOut",        .desc = "Total number of bytes emitted by current worker process through a kernel pipe since started" },
	[INF_BYTES_OUT_RATE]                 = { .name = "BytesOutRate",                .desc = "Number of bytes emitted by current worker process over the last second" },
	[INF_DEBUG_COMMANDS_ISSUED]          = { .name = "DebugCommandsIssued",         .desc = "Number of debug commands issued on this process (anything > 0 is unsafe)" },
	[INF_CUM_LOG_MSGS]                   = { .name = "CumRecvLogs",                 .desc = "Total number of log messages received by log-forwarding listeners on this worker process since started" },
	[INF_BUILD_INFO]                     = { .name = "Build info",                  .desc = "Build info" },
	[INF_TAINTED]                        = { .name = "Tainted",                     .desc = "Experimental features used" },
	[INF_WARNINGS]                       = { .name = "TotalWarnings",               .desc = "Total warnings issued" },
	[INF_MAXCONN_REACHED]                = { .name = "MaxconnReached",              .desc = "Number of times an accepted connection resulted in Maxconn being reached" },
	[INF_BOOTTIME_MS]                    = { .name = "BootTime_ms",                 .desc = "How long ago it took to parse and process the config before being ready (milliseconds)" },
};

const struct name_desc stat_fields[ST_F_TOTAL_FIELDS] = {
	[ST_F_PXNAME]                        = { .name = "pxname",                      .desc = "Proxy name" },
	[ST_F_SVNAME]                        = { .name = "svname",                      .desc = "Server name" },
	[ST_F_QCUR]                          = { .name = "qcur",                        .desc = "Number of current queued connections" },
	[ST_F_QMAX]                          = { .name = "qmax",                        .desc = "Highest value of queued connections encountered since process started" },
	[ST_F_SCUR]                          = { .name = "scur",                        .desc = "Number of current sessions on the frontend, backend or server" },
	[ST_F_SMAX]                          = { .name = "smax",                        .desc = "Highest value of current sessions encountered since process started" },
	[ST_F_SLIM]                          = { .name = "slim",                        .desc = "Frontend/listener/server's maxconn, backend's fullconn" },
	[ST_F_STOT]                          = { .name = "stot",                        .desc = "Total number of sessions since process started" },
	[ST_F_BIN]                           = { .name = "bin",                         .desc = "Total number of request bytes since process started" },
	[ST_F_BOUT]                          = { .name = "bout",                        .desc = "Total number of response bytes since process started" },
	[ST_F_DREQ]                          = { .name = "dreq",                        .desc = "Total number of denied requests since process started" },
	[ST_F_DRESP]                         = { .name = "dresp",                       .desc = "Total number of denied responses since process started" },
	[ST_F_EREQ]                          = { .name = "ereq",                        .desc = "Total number of invalid requests since process started" },
	[ST_F_ECON]                          = { .name = "econ",                        .desc = "Total number of failed connections to server since the worker process started" },
	[ST_F_ERESP]                         = { .name = "eresp",                       .desc = "Total number of invalid responses since the worker process started" },
	[ST_F_WRETR]                         = { .name = "wretr",                       .desc = "Total number of server connection retries since the worker process started" },
	[ST_F_WREDIS]                        = { .name = "wredis",                      .desc = "Total number of server redispatches due to connection failures since the worker process started" },
	[ST_F_STATUS]                        = { .name = "status",                      .desc = "Frontend/listen status: OPEN/WAITING/FULL/STOP; backend: UP/DOWN; server: last check status" },
	[ST_F_WEIGHT]                        = { .name = "weight",                      .desc = "Server's effective weight, or sum of active servers' effective weights for a backend" },
	[ST_F_ACT]                           = { .name = "act",                         .desc = "Total number of active UP servers with a non-zero weight" },
	[ST_F_BCK]                           = { .name = "bck",                         .desc = "Total number of backup UP servers with a non-zero weight" },
	[ST_F_CHKFAIL]                       = { .name = "chkfail",                     .desc = "Total number of failed individual health checks per server/backend, since the worker process started" },
	[ST_F_CHKDOWN]                       = { .name = "chkdown",                     .desc = "Total number of failed checks causing UP to DOWN server transitions, per server/backend, since the worker process started" },
	[ST_F_LASTCHG]                       = { .name = "lastchg",                     .desc = "How long ago the last server state changed, in seconds" },
	[ST_F_DOWNTIME]                      = { .name = "downtime",                    .desc = "Total time spent in DOWN state, for server or backend" },
	[ST_F_QLIMIT]                        = { .name = "qlimit",                      .desc = "Limit on the number of connections in queue, for servers only (maxqueue argument)" },
	[ST_F_PID]                           = { .name = "pid",                         .desc = "Relative worker process number (1)" },
	[ST_F_IID]                           = { .name = "iid",                         .desc = "Frontend or Backend numeric identifier ('id' setting)" },
	[ST_F_SID]                           = { .name = "sid",                         .desc = "Server numeric identifier ('id' setting)" },
	[ST_F_THROTTLE]                      = { .name = "throttle",                    .desc = "Throttling ratio applied to a server's maxconn and weight during the slowstart period (0 to 100%)" },
	[ST_F_LBTOT]                         = { .name = "lbtot",                       .desc = "Total number of requests routed by load balancing since the worker process started (ignores queue pop and stickiness)" },
	[ST_F_TRACKED]                       = { .name = "tracked",                     .desc = "Name of the other server this server tracks for its state" },
	[ST_F_TYPE]                          = { .name = "type",                        .desc = "Type of the object (Listener, Frontend, Backend, Server)" },
	[ST_F_RATE]                          = { .name = "rate",                        .desc = "Total number of sessions processed by this object over the last second (sessions for listeners/frontends, requests for backends/servers)" },
	[ST_F_RATE_LIM]                      = { .name = "rate_lim",                    .desc = "Limit on the number of sessions accepted in a second (frontend only, 'rate-limit sessions' setting)" },
	[ST_F_RATE_MAX]                      = { .name = "rate_max",                    .desc = "Highest value of sessions per second observed since the worker process started" },
	[ST_F_CHECK_STATUS]                  = { .name = "check_status",                .desc = "Status report of the server's latest health check, prefixed with '*' if a check is currently in progress" },
	[ST_F_CHECK_CODE]                    = { .name = "check_code",                  .desc = "HTTP/SMTP/LDAP status code reported by the latest server health check" },
	[ST_F_CHECK_DURATION]                = { .name = "check_duration",              .desc = "Total duration of the latest server health check, in milliseconds" },
	[ST_F_HRSP_1XX]                      = { .name = "hrsp_1xx",                    .desc = "Total number of HTTP responses with status 100-199 returned by this object since the worker process started" },
	[ST_F_HRSP_2XX]                      = { .name = "hrsp_2xx",                    .desc = "Total number of HTTP responses with status 200-299 returned by this object since the worker process started" },
	[ST_F_HRSP_3XX]                      = { .name = "hrsp_3xx",                    .desc = "Total number of HTTP responses with status 300-399 returned by this object since the worker process started" },
	[ST_F_HRSP_4XX]                      = { .name = "hrsp_4xx",                    .desc = "Total number of HTTP responses with status 400-499 returned by this object since the worker process started" },
	[ST_F_HRSP_5XX]                      = { .name = "hrsp_5xx",                    .desc = "Total number of HTTP responses with status 500-599 returned by this object since the worker process started" },
	[ST_F_HRSP_OTHER]                    = { .name = "hrsp_other",                  .desc = "Total number of HTTP responses with status <100, >599 returned by this object since the worker process started (error -1 included)" },
	[ST_F_HANAFAIL]                      = { .name = "hanafail",                    .desc = "Total number of failed checks caused by an 'on-error' directive after an 'observe' condition matched" },
	[ST_F_REQ_RATE]                      = { .name = "req_rate",                    .desc = "Number of HTTP requests processed over the last second on this object" },
	[ST_F_REQ_RATE_MAX]                  = { .name = "req_rate_max",                .desc = "Highest value of http requests observed since the worker process started" },
	[ST_F_REQ_TOT]                       = { .name = "req_tot",                     .desc = "Total number of HTTP requests processed by this object since the worker process started" },
	[ST_F_CLI_ABRT]                      = { .name = "cli_abrt",                    .desc = "Total number of requests or connections aborted by the client since the worker process started" },
	[ST_F_SRV_ABRT]                      = { .name = "srv_abrt",                    .desc = "Total number of requests or connections aborted by the server since the worker process started" },
	[ST_F_COMP_IN]                       = { .name = "comp_in",                     .desc = "Total number of bytes submitted to the HTTP compressor for this object since the worker process started" },
	[ST_F_COMP_OUT]                      = { .name = "comp_out",                    .desc = "Total number of bytes emitted by the HTTP compressor for this object since the worker process started" },
	[ST_F_COMP_BYP]                      = { .name = "comp_byp",                    .desc = "Total number of bytes that bypassed HTTP compression for this object since the worker process started (CPU/memory/bandwidth limitation)" },
	[ST_F_COMP_RSP]                      = { .name = "comp_rsp",                    .desc = "Total number of HTTP responses that were compressed for this object since the worker process started" },
	[ST_F_LASTSESS]                      = { .name = "lastsess",                    .desc = "How long ago some traffic was seen on this object on this worker process, in seconds" },
	[ST_F_LAST_CHK]                      = { .name = "last_chk",                    .desc = "Short description of the latest health check report for this server (see also check_desc)" },
	[ST_F_LAST_AGT]                      = { .name = "last_agt",                    .desc = "Short description of the latest agent check report for this server (see also agent_desc)" },
	[ST_F_QTIME]                         = { .name = "qtime",                       .desc = "Time spent in the queue, in milliseconds, averaged over the 1024 last requests (backend/server)" },
	[ST_F_CTIME]                         = { .name = "ctime",                       .desc = "Time spent waiting for a connection to complete, in milliseconds, averaged over the 1024 last requests (backend/server)" },
	[ST_F_RTIME]                         = { .name = "rtime",                       .desc = "Time spent waiting for a server response, in milliseconds, averaged over the 1024 last requests (backend/server)" },
	[ST_F_TTIME]                         = { .name = "ttime",                       .desc = "Total request+response time (request+queue+connect+response+processing), in milliseconds, averaged over the 1024 last requests (backend/server)" },
	[ST_F_AGENT_STATUS]                  = { .name = "agent_status",                .desc = "Status report of the server's latest agent check, prefixed with '*' if a check is currently in progress" },
	[ST_F_AGENT_CODE]                    = { .name = "agent_code",                  .desc = "Status code reported by the latest server agent check" },
	[ST_F_AGENT_DURATION]                = { .name = "agent_duration",              .desc = "Total duration of the latest server agent check, in milliseconds" },
	[ST_F_CHECK_DESC]                    = { .name = "check_desc",                  .desc = "Textual description of the latest health check report for this server" },
	[ST_F_AGENT_DESC]                    = { .name = "agent_desc",                  .desc = "Textual description of the latest agent check report for this server" },
	[ST_F_CHECK_RISE]                    = { .name = "check_rise",                  .desc = "Number of successful health checks before declaring a server UP (server 'rise' setting)" },
	[ST_F_CHECK_FALL]                    = { .name = "check_fall",                  .desc = "Number of failed health checks before declaring a server DOWN (server 'fall' setting)" },
	[ST_F_CHECK_HEALTH]                  = { .name = "check_health",                .desc = "Current server health check level (0..fall-1=DOWN, fall..rise-1=UP)" },
	[ST_F_AGENT_RISE]                    = { .name = "agent_rise",                  .desc = "Number of successful agent checks before declaring a server UP (server 'rise' setting)" },
	[ST_F_AGENT_FALL]                    = { .name = "agent_fall",                  .desc = "Number of failed agent checks before declaring a server DOWN (server 'fall' setting)" },
	[ST_F_AGENT_HEALTH]                  = { .name = "agent_health",                .desc = "Current server agent check level (0..fall-1=DOWN, fall..rise-1=UP)" },
	[ST_F_ADDR]                          = { .name = "addr",                        .desc = "Server's address:port, shown only if show-legends is set, or at levels oper/admin for the CLI" },
	[ST_F_COOKIE]                        = { .name = "cookie",                      .desc = "Backend's cookie name or Server's cookie value, shown only if show-legends is set, or at levels oper/admin for the CLI" },
	[ST_F_MODE]                          = { .name = "mode",                        .desc = "'mode' setting (tcp/http/health/cli)" },
	[ST_F_ALGO]                          = { .name = "algo",                        .desc = "Backend's load balancing algorithm, shown only if show-legends is set, or at levels oper/admin for the CLI" },
	[ST_F_CONN_RATE]                     = { .name = "conn_rate",                   .desc = "Number of new connections accepted over the last second on the frontend for this worker process" },
	[ST_F_CONN_RATE_MAX]                 = { .name = "conn_rate_max",               .desc = "Highest value of connections per second observed since the worker process started" },
	[ST_F_CONN_TOT]                      = { .name = "conn_tot",                    .desc = "Total number of new connections accepted on this frontend since the worker process started" },
	[ST_F_INTERCEPTED]                   = { .name = "intercepted",                 .desc = "Total number of HTTP requests intercepted on the frontend (redirects/stats/services) since the worker process started" },
	[ST_F_DCON]                          = { .name = "dcon",                        .desc = "Total number of incoming connections blocked on a listener/frontend by a tcp-request connection rule since the worker process started" },
	[ST_F_DSES]                          = { .name = "dses",                        .desc = "Total number of incoming sessions blocked on a listener/frontend by a tcp-request connection rule since the worker process started" },
	[ST_F_WREW]                          = { .name = "wrew",                        .desc = "Total number of failed HTTP header rewrites since the worker process started" },
	[ST_F_CONNECT]                       = { .name = "connect",                     .desc = "Total number of outgoing connection attempts on this backend/server since the worker process started" },
	[ST_F_REUSE]                         = { .name = "reuse",                       .desc = "Total number of reused connection on this backend/server since the worker process started" },
	[ST_F_CACHE_LOOKUPS]                 = { .name = "cache_lookups",               .desc = "Total number of HTTP requests looked up in the cache on this frontend/backend since the worker process started" },
	[ST_F_CACHE_HITS]                    = { .name = "cache_hits",                  .desc = "Total number of HTTP requests not found in the cache on this frontend/backend since the worker process started" },
	[ST_F_SRV_ICUR]                      = { .name = "srv_icur",                    .desc = "Current number of idle connections available for reuse on this server" },
	[ST_F_SRV_ILIM]                      = { .name = "src_ilim",                    .desc = "Limit on the number of available idle connections on this server (server 'pool_max_conn' directive)" },
	[ST_F_QT_MAX]                        = { .name = "qtime_max",                   .desc = "Maximum observed time spent in the queue, in milliseconds (backend/server)" },
	[ST_F_CT_MAX]                        = { .name = "ctime_max",                   .desc = "Maximum observed time spent waiting for a connection to complete, in milliseconds (backend/server)" },
	[ST_F_RT_MAX]                        = { .name = "rtime_max",                   .desc = "Maximum observed time spent waiting for a server response, in milliseconds (backend/server)" },
	[ST_F_TT_MAX]                        = { .name = "ttime_max",                   .desc = "Maximum observed total request+response time (request+queue+connect+response+processing), in milliseconds (backend/server)" },
	[ST_F_EINT]                          = { .name = "eint",                        .desc = "Total number of internal errors since process started"},
	[ST_F_IDLE_CONN_CUR]                 = { .name = "idle_conn_cur",               .desc = "Current number of unsafe idle connections"},
	[ST_F_SAFE_CONN_CUR]                 = { .name = "safe_conn_cur",               .desc = "Current number of safe idle connections"},
	[ST_F_USED_CONN_CUR]                 = { .name = "used_conn_cur",               .desc = "Current number of connections in use"},
	[ST_F_NEED_CONN_EST]                 = { .name = "need_conn_est",               .desc = "Estimated needed number of connections"},
	[ST_F_UWEIGHT]                       = { .name = "uweight",                     .desc = "Server's user weight, or sum of active servers' user weights for a backend" },
	[ST_F_AGG_SRV_CHECK_STATUS]          = { .name = "agg_server_check_status",     .desc = "[DEPRECATED] Backend's aggregated gauge of servers' status" },
	[ST_F_AGG_SRV_STATUS ]               = { .name = "agg_server_status",           .desc = "Backend's aggregated gauge of servers' status" },
	[ST_F_AGG_CHECK_STATUS]              = { .name = "agg_check_status",            .desc = "Backend's aggregated gauge of servers' state check status" },
	[ST_F_SRID]                          = { .name = "srid",                        .desc = "Server id revision, to prevent server id reuse mixups" },
	[ST_F_SESS_OTHER]                    = { .name = "sess_other",                  .desc = "Total number of sessions other than HTTP since process started" },
	[ST_F_H1SESS]                        = { .name = "h1sess",                      .desc = "Total number of HTTP/1 sessions since process started" },
	[ST_F_H2SESS]                        = { .name = "h2sess",                      .desc = "Total number of HTTP/2 sessions since process started" },
	[ST_F_H3SESS]                        = { .name = "h3sess",                      .desc = "Total number of HTTP/3 sessions since process started" },
	[ST_F_REQ_OTHER]                     = { .name = "req_other",                   .desc = "Total number of sessions other than HTTP processed by this object since the worker process started" },
	[ST_F_H1REQ]                         = { .name = "h1req",                       .desc = "Total number of HTTP/1 sessions processed by this object since the worker process started" },
	[ST_F_H2REQ]                         = { .name = "h2req",                       .desc = "Total number of hTTP/2 sessions processed by this object since the worker process started" },
	[ST_F_H3REQ]                         = { .name = "h3req",                       .desc = "Total number of HTTP/3 sessions processed by this object since the worker process started" },
	[ST_F_PROTO]                         = { .name = "proto",                       .desc = "Protocol" },
};

/* one line of info */
THREAD_LOCAL struct field info[INF_TOTAL_FIELDS];

/* description of statistics (static and dynamic) */
static struct name_desc *stat_f[STATS_DOMAIN_COUNT];
static size_t stat_count[STATS_DOMAIN_COUNT];

/* one line for stats */
THREAD_LOCAL struct field *stat_l[STATS_DOMAIN_COUNT];

/* list of all registered stats module */
static struct list stats_module_list[STATS_DOMAIN_COUNT] = {
	LIST_HEAD_INIT(stats_module_list[STATS_DOMAIN_PROXY]),
	LIST_HEAD_INIT(stats_module_list[STATS_DOMAIN_RESOLVERS]),
};

THREAD_LOCAL void *trash_counters;
static THREAD_LOCAL struct buffer trash_chunk = BUF_NULL;


static inline uint8_t stats_get_domain(uint32_t domain)
{
	return domain >> STATS_DOMAIN & STATS_DOMAIN_MASK;
}

static inline enum stats_domain_px_cap stats_px_get_cap(uint32_t domain)
{
	return domain >> STATS_PX_CAP & STATS_PX_CAP_MASK;
}

static void stats_dump_json_schema(struct buffer *out);

int stats_putchk(struct appctx *appctx, struct htx *htx)
{
	struct stconn *sc = appctx_sc(appctx);
	struct channel *chn = sc_ic(sc);
	struct buffer *chk = &trash_chunk;

	if (htx) {
		if (chk->data >= channel_htx_recv_max(chn, htx)) {
			sc_need_room(sc, chk->data);
			return 0;
		}
		if (!htx_add_data_atonce(htx, ist2(chk->area, chk->data))) {
			sc_need_room(sc, 0);
			return 0;
		}
		channel_add_input(chn, chk->data);
		chk->data = 0;
	}
	else  {
		if (applet_putchk(appctx, chk) == -1)
			return 0;
	}
	return 1;
}

static const char *stats_scope_ptr(struct appctx *appctx, struct stconn *sc)
{
	struct show_stat_ctx *ctx = appctx->svcctx;
	struct channel *req = sc_oc(sc);
	struct htx *htx = htxbuf(&req->buf);
	struct htx_blk *blk;
	struct ist uri;

	blk = htx_get_head_blk(htx);
	BUG_ON(!blk || htx_get_blk_type(blk) != HTX_BLK_REQ_SL);
	ALREADY_CHECKED(blk);
	uri = htx_sl_req_uri(htx_get_blk_ptr(htx, blk));
	return uri.ptr + ctx->scope_str;
}

/*
 * http_stats_io_handler()
 *     -> stats_dump_stat_to_buffer()     // same as above, but used for CSV or HTML
 *        -> stats_dump_csv_header()      // emits the CSV headers (same as above)
 *        -> stats_dump_json_header()     // emits the JSON headers (same as above)
 *        -> stats_dump_html_head()       // emits the HTML headers
 *        -> stats_dump_html_info()       // emits the equivalent of "show info" at the top
 *        -> stats_dump_proxy_to_buffer() // same as above, valid for CSV and HTML
 *           -> stats_dump_html_px_hdr()
 *           -> stats_dump_fe_stats()
 *           -> stats_dump_li_stats()
 *           -> stats_dump_sv_stats()
 *           -> stats_dump_be_stats()
 *           -> stats_dump_html_px_end()
 *        -> stats_dump_html_end()       // emits HTML trailer
 *        -> stats_dump_json_end()       // emits JSON trailer
 */


/* Dumps the stats CSV header to the local trash buffer. The caller is
 * responsible for clearing it if needed.
 * NOTE: Some tools happen to rely on the field position instead of its name,
 *       so please only append new fields at the end, never in the middle.
 */
static void stats_dump_csv_header(enum stats_domain domain)
{
	int field;

	chunk_appendf(&trash_chunk, "# ");
	if (stat_f[domain]) {
		for (field = 0; field < stat_count[domain]; ++field) {
			chunk_appendf(&trash_chunk, "%s,", stat_f[domain][field].name);

			/* print special delimiter on proxy stats to mark end of
			   static fields */
			if (domain == STATS_DOMAIN_PROXY && field + 1 == ST_F_TOTAL_FIELDS)
				chunk_appendf(&trash_chunk, "-,");
		}
	}

	chunk_appendf(&trash_chunk, "\n");
}

/* Emits a stats field without any surrounding element and properly encoded to
 * resist CSV output. Returns non-zero on success, 0 if the buffer is full.
 */
int stats_emit_raw_data_field(struct buffer *out, const struct field *f)
{
	switch (field_format(f, 0)) {
	case FF_EMPTY: return 1;
	case FF_S32:   return chunk_appendf(out, "%d", f->u.s32);
	case FF_U32:   return chunk_appendf(out, "%u", f->u.u32);
	case FF_S64:   return chunk_appendf(out, "%lld", (long long)f->u.s64);
	case FF_U64:   return chunk_appendf(out, "%llu", (unsigned long long)f->u.u64);
	case FF_FLT:   {
		size_t prev_data = out->data;
		out->data = flt_trim(out->area, prev_data, chunk_appendf(out, "%f", f->u.flt));
		return out->data;
	}
	case FF_STR:   return csv_enc_append(field_str(f, 0), 1, out) != NULL;
	default:       return chunk_appendf(out, "[INCORRECT_FIELD_TYPE_%08x]", f->type);
	}
}

const char *field_to_html_str(const struct field *f)
{
	switch (field_format(f, 0)) {
	case FF_S32: return U2H(f->u.s32);
	case FF_S64: return U2H(f->u.s64);
	case FF_U64: return U2H(f->u.u64);
	case FF_U32: return U2H(f->u.u32);
	case FF_FLT: return F2H(f->u.flt);
	case FF_STR: return field_str(f, 0);
	case FF_EMPTY:
	default:
		return "";
	}
}

/* Emits a stats field prefixed with its type. No CSV encoding is prepared, the
 * output is supposed to be used on its own line. Returns non-zero on success, 0
 * if the buffer is full.
 */
int stats_emit_typed_data_field(struct buffer *out, const struct field *f)
{
	switch (field_format(f, 0)) {
	case FF_EMPTY: return 1;
	case FF_S32:   return chunk_appendf(out, "s32:%d", f->u.s32);
	case FF_U32:   return chunk_appendf(out, "u32:%u", f->u.u32);
	case FF_S64:   return chunk_appendf(out, "s64:%lld", (long long)f->u.s64);
	case FF_U64:   return chunk_appendf(out, "u64:%llu", (unsigned long long)f->u.u64);
	case FF_FLT:   {
		size_t prev_data = out->data;
		out->data = flt_trim(out->area, prev_data, chunk_appendf(out, "flt:%f", f->u.flt));
		return out->data;
	}
	case FF_STR:   return chunk_appendf(out, "str:%s", field_str(f, 0));
	default:       return chunk_appendf(out, "%08x:?", f->type);
	}
}

/* Limit JSON integer values to the range [-(2**53)+1, (2**53)-1] as per
 * the recommendation for interoperable integers in section 6 of RFC 7159.
 */
#define JSON_INT_MAX ((1ULL << 53) - 1)
#define JSON_INT_MIN (0 - JSON_INT_MAX)

/* Emits a stats field value and its type in JSON.
 * Returns non-zero on success, 0 on error.
 */
int stats_emit_json_data_field(struct buffer *out, const struct field *f)
{
	int old_len;
	char buf[20];
	const char *type, *value = buf, *quote = "";

	switch (field_format(f, 0)) {
	case FF_EMPTY: return 1;
	case FF_S32:   type = "\"s32\"";
		       snprintf(buf, sizeof(buf), "%d", f->u.s32);
		       break;
	case FF_U32:   type = "\"u32\"";
		       snprintf(buf, sizeof(buf), "%u", f->u.u32);
		       break;
	case FF_S64:   type = "\"s64\"";
		       if (f->u.s64 < JSON_INT_MIN || f->u.s64 > JSON_INT_MAX)
			       return 0;
		       type = "\"s64\"";
		       snprintf(buf, sizeof(buf), "%lld", (long long)f->u.s64);
		       break;
	case FF_U64:   if (f->u.u64 > JSON_INT_MAX)
			       return 0;
		       type = "\"u64\"";
		       snprintf(buf, sizeof(buf), "%llu",
				(unsigned long long) f->u.u64);
		       break;
	case FF_FLT:   type = "\"flt\"";
		       flt_trim(buf, 0, snprintf(buf, sizeof(buf), "%f", f->u.flt));
		       break;
	case FF_STR:   type = "\"str\"";
		       value = field_str(f, 0);
		       quote = "\"";
		       break;
	default:       snprintf(buf, sizeof(buf), "%u", f->type);
		       type = buf;
		       value = "unknown";
		       quote = "\"";
		       break;
	}

	old_len = out->data;
	chunk_appendf(out, ",\"value\":{\"type\":%s,\"value\":%s%s%s}",
		      type, quote, value, quote);
	return !(old_len == out->data);
}

/* Emits an encoding of the field type on 3 characters followed by a delimiter.
 * Returns non-zero on success, 0 if the buffer is full.
 */
int stats_emit_field_tags(struct buffer *out, const struct field *f,
			  char delim)
{
	char origin, nature, scope;

	switch (field_origin(f, 0)) {
	case FO_METRIC:  origin = 'M'; break;
	case FO_STATUS:  origin = 'S'; break;
	case FO_KEY:     origin = 'K'; break;
	case FO_CONFIG:  origin = 'C'; break;
	case FO_PRODUCT: origin = 'P'; break;
	default:         origin = '?'; break;
	}

	switch (field_nature(f, 0)) {
	case FN_GAUGE:    nature = 'G'; break;
	case FN_LIMIT:    nature = 'L'; break;
	case FN_MIN:      nature = 'm'; break;
	case FN_MAX:      nature = 'M'; break;
	case FN_RATE:     nature = 'R'; break;
	case FN_COUNTER:  nature = 'C'; break;
	case FN_DURATION: nature = 'D'; break;
	case FN_AGE:      nature = 'A'; break;
	case FN_TIME:     nature = 'T'; break;
	case FN_NAME:     nature = 'N'; break;
	case FN_OUTPUT:   nature = 'O'; break;
	case FN_AVG:      nature = 'a'; break;
	default:          nature = '?'; break;
	}

	switch (field_scope(f, 0)) {
	case FS_PROCESS: scope = 'P'; break;
	case FS_SERVICE: scope = 'S'; break;
	case FS_SYSTEM:  scope = 's'; break;
	case FS_CLUSTER: scope = 'C'; break;
	default:         scope = '?'; break;
	}

	return chunk_appendf(out, "%c%c%c%c", origin, nature, scope, delim);
}

/* Emits an encoding of the field type as JSON.
  * Returns non-zero on success, 0 if the buffer is full.
  */
int stats_emit_json_field_tags(struct buffer *out, const struct field *f)
{
	const char *origin, *nature, *scope;
	int old_len;

	switch (field_origin(f, 0)) {
	case FO_METRIC:  origin = "Metric";  break;
	case FO_STATUS:  origin = "Status";  break;
	case FO_KEY:     origin = "Key";     break;
	case FO_CONFIG:  origin = "Config";  break;
	case FO_PRODUCT: origin = "Product"; break;
	default:         origin = "Unknown"; break;
	}

	switch (field_nature(f, 0)) {
	case FN_GAUGE:    nature = "Gauge";    break;
	case FN_LIMIT:    nature = "Limit";    break;
	case FN_MIN:      nature = "Min";      break;
	case FN_MAX:      nature = "Max";      break;
	case FN_RATE:     nature = "Rate";     break;
	case FN_COUNTER:  nature = "Counter";  break;
	case FN_DURATION: nature = "Duration"; break;
	case FN_AGE:      nature = "Age";      break;
	case FN_TIME:     nature = "Time";     break;
	case FN_NAME:     nature = "Name";     break;
	case FN_OUTPUT:   nature = "Output";   break;
	case FN_AVG:      nature = "Avg";      break;
	default:          nature = "Unknown";  break;
	}

	switch (field_scope(f, 0)) {
	case FS_PROCESS: scope = "Process"; break;
	case FS_SERVICE: scope = "Service"; break;
	case FS_SYSTEM:  scope = "System";  break;
	case FS_CLUSTER: scope = "Cluster"; break;
	default:         scope = "Unknown"; break;
	}

	old_len = out->data;
	chunk_appendf(out, "\"tags\":{"
			    "\"origin\":\"%s\","
			    "\"nature\":\"%s\","
			    "\"scope\":\"%s\""
			   "}", origin, nature, scope);
	return !(old_len == out->data);
}

/* Dump all fields from <stats> into <out> using CSV format */
static int stats_dump_fields_csv(struct buffer *out,
                                 const struct field *stats, size_t stats_count,
                                 struct show_stat_ctx *ctx)
{
	int domain = ctx->domain;
	int field;

	for (field = 0; field < stats_count; ++field) {
		if (!stats_emit_raw_data_field(out, &stats[field]))
			return 0;
		if (!chunk_strcat(out, ","))
			return 0;

		/* print special delimiter on proxy stats to mark end of
		   static fields */
		if (domain == STATS_DOMAIN_PROXY && field + 1 == ST_F_TOTAL_FIELDS) {
			if (!chunk_strcat(out, "-,"))
				return 0;
		}
	}

	chunk_strcat(out, "\n");
	return 1;
}

/* Dump all fields from <stats> into <out> using a typed "field:desc:type:value" format */
static int stats_dump_fields_typed(struct buffer *out,
                                   const struct field *stats,
                                   size_t stats_count,
                                   struct show_stat_ctx * ctx)
{
	int flags = ctx->flags;
	int domain = ctx->domain;
	int field;

	for (field = 0; field < stats_count; ++field) {
		if (!stats[field].type)
			continue;

		switch (domain) {
		case STATS_DOMAIN_PROXY:
			chunk_appendf(out, "%c.%u.%u.%d.%s.%u:",
			              stats[ST_F_TYPE].u.u32 == STATS_TYPE_FE ? 'F' :
			              stats[ST_F_TYPE].u.u32 == STATS_TYPE_BE ? 'B' :
			              stats[ST_F_TYPE].u.u32 == STATS_TYPE_SO ? 'L' :
			              stats[ST_F_TYPE].u.u32 == STATS_TYPE_SV ? 'S' :
			              '?',
			              stats[ST_F_IID].u.u32, stats[ST_F_SID].u.u32,
			              field,
			              stat_f[domain][field].name,
			              stats[ST_F_PID].u.u32);
			break;

		case STATS_DOMAIN_RESOLVERS:
			chunk_appendf(out, "N.%d.%s:", field,
			              stat_f[domain][field].name);
			break;

		default:
			break;
		}

		if (!stats_emit_field_tags(out, &stats[field], ':'))
			return 0;
		if (!stats_emit_typed_data_field(out, &stats[field]))
			return 0;

		if (flags & STAT_SHOW_FDESC &&
		    !chunk_appendf(out, ":\"%s\"", stat_f[domain][field].desc)) {
			return 0;
		}

		if (!chunk_strcat(out, "\n"))
			return 0;
	}
	return 1;
}

/* Dump all fields from <stats> into <out> using the "show info json" format */
static int stats_dump_json_info_fields(struct buffer *out,
                                       const struct field *info,
                                       struct show_stat_ctx *ctx)
{
	int started = (ctx->field) ? 1 : 0;
	int ready_data = 0;

	if (!started && !chunk_strcat(out, "["))
		return 0;

	for (; ctx->field < INF_TOTAL_FIELDS; ctx->field++) {
		int old_len;
		int field = ctx->field;

		if (!field_format(info, field))
			continue;

		if (started && !chunk_strcat(out, ","))
			goto err;
		started = 1;

		old_len = out->data;
		chunk_appendf(out,
			      "{\"field\":{\"pos\":%d,\"name\":\"%s\"},"
			      "\"processNum\":%u,",
			      field, info_fields[field].name,
			      info[INF_PROCESS_NUM].u.u32);
		if (old_len == out->data)
			goto err;

		if (!stats_emit_json_field_tags(out, &info[field]))
			goto err;

		if (!stats_emit_json_data_field(out, &info[field]))
			goto err;

		if (!chunk_strcat(out, "}"))
			goto err;
		ready_data = out->data;
	}

	if (!chunk_strcat(out, "]\n"))
		goto err;
	ctx->field = 0; /* we're done */
	return 1;

err:
	if (!ready_data) {
		/* not enough buffer space for a single entry.. */
		chunk_reset(out);
		chunk_appendf(out, "{\"errorStr\":\"output buffer too short\"}\n");
		return 0; /* hard error */
	}
	/* push ready data and wait for a new buffer to complete the dump */
	out->data = ready_data;
	return 1;
}

static void stats_print_proxy_field_json(struct buffer *out,
                                         const struct field *stat,
                                         const char *name,
                                         int pos,
                                         uint32_t field_type,
                                         uint32_t iid,
                                         uint32_t sid,
                                         uint32_t pid)
{
	const char *obj_type;
	switch (field_type) {
		case STATS_TYPE_FE: obj_type = "Frontend"; break;
		case STATS_TYPE_BE: obj_type = "Backend";  break;
		case STATS_TYPE_SO: obj_type = "Listener"; break;
		case STATS_TYPE_SV: obj_type = "Server";   break;
		default:            obj_type = "Unknown";  break;
	}

	chunk_appendf(out,
	              "{"
	              "\"objType\":\"%s\","
	              "\"proxyId\":%u,"
	              "\"id\":%u,"
	              "\"field\":{\"pos\":%d,\"name\":\"%s\"},"
	              "\"processNum\":%u,",
	              obj_type, iid, sid, pos, name, pid);
}

static void stats_print_rslv_field_json(struct buffer *out,
                                        const struct field *stat,
                                        const char *name,
                                        int pos)
{
	chunk_appendf(out,
	              "{"
	              "\"field\":{\"pos\":%d,\"name\":\"%s\"},",
	              pos, name);
}


/* Dump all fields from <stats> into <out> using a typed "field:desc:type:value" format */
static int stats_dump_fields_json(struct buffer *out,
                                  const struct field *stats, size_t stats_count,
                                  struct show_stat_ctx *ctx)
{
	int flags = ctx->flags;
	int domain = ctx->domain;
	int started = (ctx->field) ? 1 : 0;
	int ready_data = 0;

	if (!started && (flags & STAT_STARTED) && !chunk_strcat(out, ","))
		return 0;
	if (!started && !chunk_strcat(out, "["))
		return 0;

	for (; ctx->field < stats_count; ctx->field++) {
		int old_len;
		int field = ctx->field;

		if (!stats[field].type)
			continue;

		if (started && !chunk_strcat(out, ","))
			goto err;
		started = 1;

		old_len = out->data;
		if (domain == STATS_DOMAIN_PROXY) {
			stats_print_proxy_field_json(out, &stats[field],
			                             stat_f[domain][field].name,
			                             field,
			                             stats[ST_F_TYPE].u.u32,
			                             stats[ST_F_IID].u.u32,
			                             stats[ST_F_SID].u.u32,
			                             stats[ST_F_PID].u.u32);
		} else if (domain == STATS_DOMAIN_RESOLVERS) {
			stats_print_rslv_field_json(out, &stats[field],
			                            stat_f[domain][field].name,
			                            field);
		}

		if (old_len == out->data)
			goto err;

		if (!stats_emit_json_field_tags(out, &stats[field]))
			goto err;

		if (!stats_emit_json_data_field(out, &stats[field]))
			goto err;

		if (!chunk_strcat(out, "}"))
			goto err;
		ready_data = out->data;
	}

	if (!chunk_strcat(out, "]"))
		goto err;

	ctx->field = 0; /* we're done */
	return 1;

err:
	if (!ready_data) {
		/* not enough buffer space for a single entry.. */
		chunk_reset(out);
		if (ctx->flags & STAT_STARTED)
			chunk_strcat(out, ",");
		chunk_appendf(out, "{\"errorStr\":\"output buffer too short\"}");
		return 0; /* hard error */
	}
	/* push ready data and wait for a new buffer to complete the dump */
	out->data = ready_data;
	return 1;
}

/* Dump all fields from <stats> into <out> using the HTML format. A column is
 * reserved for the checkbox is STAT_ADMIN is set in <flags>. Some extra info
 * are provided if STAT_SHLGNDS is present in <flags>. The statistics from
 * extra modules are displayed at the end of the lines if STAT_SHMODULES is
 * present in <flags>.
 */
static int stats_dump_fields_html(struct buffer *out,
                                  const struct field *stats,
                                  struct show_stat_ctx *ctx)
{
	struct buffer src;
	struct stats_module *mod;
	int flags = ctx->flags;
	int i = 0, j = 0;

	if (stats[ST_F_TYPE].u.u32 == STATS_TYPE_FE) {
		chunk_appendf(out,
		              /* name, queue */
		              "<tr class=\"frontend\">");

		if (flags & STAT_ADMIN) {
			/* Column sub-heading for Enable or Disable server */
			chunk_appendf(out, "<td></td>");
		}

		chunk_appendf(out,
		              "<td class=ac>"
		              "<a name=\"%s/Frontend\"></a>"
		              "<a class=lfsb href=\"#%s/Frontend\">Frontend</a></td>"
		              "<td colspan=3></td>"
		              "",
		              field_str(stats, ST_F_PXNAME), field_str(stats, ST_F_PXNAME));

		chunk_appendf(out,
		              /* sessions rate : current */
		              "<td><u>%s<div class=tips><table class=det>"
		              "<tr><th>Current connection rate:</th><td>%s/s</td></tr>"
		              "<tr><th>Current session rate:</th><td>%s/s</td></tr>"
		              "",
		              U2H(stats[ST_F_RATE].u.u32),
		              U2H(stats[ST_F_CONN_RATE].u.u32),
		              U2H(stats[ST_F_RATE].u.u32));

		if (strcmp(field_str(stats, ST_F_MODE), "http") == 0)
			chunk_appendf(out,
			              "<tr><th>Current request rate:</th><td>%s/s</td></tr>",
			              U2H(stats[ST_F_REQ_RATE].u.u32));

		chunk_appendf(out,
		              "</table></div></u></td>"
		              /* sessions rate : max */
		              "<td><u>%s<div class=tips><table class=det>"
		              "<tr><th>Max connection rate:</th><td>%s/s</td></tr>"
		              "<tr><th>Max session rate:</th><td>%s/s</td></tr>"
		              "",
		              U2H(stats[ST_F_RATE_MAX].u.u32),
		              U2H(stats[ST_F_CONN_RATE_MAX].u.u32),
		              U2H(stats[ST_F_RATE_MAX].u.u32));

		if (strcmp(field_str(stats, ST_F_MODE), "http") == 0)
			chunk_appendf(out,
			              "<tr><th>Max request rate:</th><td>%s/s</td></tr>",
			              U2H(stats[ST_F_REQ_RATE_MAX].u.u32));

		chunk_appendf(out,
		              "</table></div></u></td>"
		              /* sessions rate : limit */
		              "<td>%s</td>",
		              LIM2A(stats[ST_F_RATE_LIM].u.u32, "-"));

		chunk_appendf(out,
		              /* sessions: current, max, limit, total */
		              "<td>%s</td><td>%s</td><td>%s</td>"
		              "<td><u>%s<div class=tips><table class=det>"
		              "<tr><th>Cum. connections:</th><td>%s</td></tr>"
		              "<tr><th>Cum. sessions:</th><td>%s</td></tr>"
		              "",
		              U2H(stats[ST_F_SCUR].u.u32), U2H(stats[ST_F_SMAX].u.u32), U2H(stats[ST_F_SLIM].u.u32),
		              U2H(stats[ST_F_STOT].u.u64),
		              U2H(stats[ST_F_CONN_TOT].u.u64),
		              U2H(stats[ST_F_STOT].u.u64));

		/* http response (via hover): 1xx, 2xx, 3xx, 4xx, 5xx, other */
		if (strcmp(field_str(stats, ST_F_MODE), "http") == 0) {
			chunk_appendf(out,
			              "<tr><th>- HTTP/1 sessions:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP/2 sessions:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP/3 sessions:</th><td>%s</td></tr>"
			              "<tr><th>- other sessions:</th><td>%s</td></tr>"
			              "<tr><th>Cum. HTTP requests:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP/1 requests:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP/2 requests:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP/3 requests:</th><td>%s</td></tr>"
			              "<tr><th>- other requests:</th><td>%s</td></tr>"
			              "",
			              U2H(stats[ST_F_H1SESS].u.u64),
			              U2H(stats[ST_F_H2SESS].u.u64),
			              U2H(stats[ST_F_H3SESS].u.u64),
			              U2H(stats[ST_F_SESS_OTHER].u.u64),
			              U2H(stats[ST_F_REQ_TOT].u.u64),
			              U2H(stats[ST_F_H1REQ].u.u64),
			              U2H(stats[ST_F_H2REQ].u.u64),
			              U2H(stats[ST_F_H3REQ].u.u64),
			              U2H(stats[ST_F_REQ_OTHER].u.u64));

			chunk_appendf(out,
			              "<tr><th>- HTTP 1xx responses:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 2xx responses:</th><td>%s</td></tr>"
			              "<tr><th>&nbsp;&nbsp;Compressed 2xx:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>- HTTP 3xx responses:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 4xx responses:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 5xx responses:</th><td>%s</td></tr>"
			              "<tr><th>- other responses:</th><td>%s</td></tr>"
			              "",
			              U2H(stats[ST_F_HRSP_1XX].u.u64),
			              U2H(stats[ST_F_HRSP_2XX].u.u64),
			              U2H(stats[ST_F_COMP_RSP].u.u64),
			              stats[ST_F_HRSP_2XX].u.u64 ?
			              (int)(100 * stats[ST_F_COMP_RSP].u.u64 / stats[ST_F_HRSP_2XX].u.u64) : 0,
			              U2H(stats[ST_F_HRSP_3XX].u.u64),
			              U2H(stats[ST_F_HRSP_4XX].u.u64),
			              U2H(stats[ST_F_HRSP_5XX].u.u64),
			              U2H(stats[ST_F_HRSP_OTHER].u.u64));

			chunk_appendf(out,
			              "<tr><th>Intercepted requests:</th><td>%s</td></tr>"
			              "<tr><th>Cache lookups:</th><td>%s</td></tr>"
			              "<tr><th>Cache hits:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>Failed hdr rewrites:</th><td>%s</td></tr>"
			              "<tr><th>Internal errors:</th><td>%s</td></tr>"
			              "",
			              U2H(stats[ST_F_INTERCEPTED].u.u64),
			              U2H(stats[ST_F_CACHE_LOOKUPS].u.u64),
			              U2H(stats[ST_F_CACHE_HITS].u.u64),
			              stats[ST_F_CACHE_LOOKUPS].u.u64 ?
			              (int)(100 * stats[ST_F_CACHE_HITS].u.u64 / stats[ST_F_CACHE_LOOKUPS].u.u64) : 0,
			              U2H(stats[ST_F_WREW].u.u64),
			              U2H(stats[ST_F_EINT].u.u64));
		}

		chunk_appendf(out,
		              "</table></div></u></td>"
		              /* sessions: lbtot, lastsess */
		              "<td></td><td></td>"
		              /* bytes : in */
		              "<td>%s</td>"
		              "",
		              U2H(stats[ST_F_BIN].u.u64));

		chunk_appendf(out,
			      /* bytes:out + compression stats (via hover): comp_in, comp_out, comp_byp */
		              "<td>%s%s<div class=tips><table class=det>"
			      "<tr><th>Response bytes in:</th><td>%s</td></tr>"
			      "<tr><th>Compression in:</th><td>%s</td></tr>"
			      "<tr><th>Compression out:</th><td>%s</td><td>(%d%%)</td></tr>"
			      "<tr><th>Compression bypass:</th><td>%s</td></tr>"
			      "<tr><th>Total bytes saved:</th><td>%s</td><td>(%d%%)</td></tr>"
			      "</table></div>%s</td>",
		              (stats[ST_F_COMP_IN].u.u64 || stats[ST_F_COMP_BYP].u.u64) ? "<u>":"",
		              U2H(stats[ST_F_BOUT].u.u64),
		              U2H(stats[ST_F_BOUT].u.u64),
		              U2H(stats[ST_F_COMP_IN].u.u64),
			      U2H(stats[ST_F_COMP_OUT].u.u64),
			      stats[ST_F_COMP_IN].u.u64 ? (int)(stats[ST_F_COMP_OUT].u.u64 * 100 / stats[ST_F_COMP_IN].u.u64) : 0,
			      U2H(stats[ST_F_COMP_BYP].u.u64),
			      U2H(stats[ST_F_COMP_IN].u.u64 - stats[ST_F_COMP_OUT].u.u64),
			      stats[ST_F_BOUT].u.u64 ? (int)((stats[ST_F_COMP_IN].u.u64 - stats[ST_F_COMP_OUT].u.u64) * 100 / stats[ST_F_BOUT].u.u64) : 0,
		              (stats[ST_F_COMP_IN].u.u64 || stats[ST_F_COMP_BYP].u.u64) ? "</u>":"");

		chunk_appendf(out,
		              /* denied: req, resp */
		              "<td>%s</td><td>%s</td>"
		              /* errors : request, connect, response */
		              "<td>%s</td><td></td><td></td>"
		              /* warnings: retries, redispatches */
		              "<td></td><td></td>"
		              /* server status : reflect frontend status */
		              "<td class=ac>%s</td>"
		              /* rest of server: nothing */
		              "<td class=ac colspan=8></td>"
		              "",
		              U2H(stats[ST_F_DREQ].u.u64), U2H(stats[ST_F_DRESP].u.u64),
		              U2H(stats[ST_F_EREQ].u.u64),
		              field_str(stats, ST_F_STATUS));

		if (flags & STAT_SHMODULES) {
			list_for_each_entry(mod, &stats_module_list[STATS_DOMAIN_PROXY], list) {
				chunk_appendf(out, "<td>");

				if (stats_px_get_cap(mod->domain_flags) & STATS_PX_CAP_FE) {
					chunk_appendf(out,
					              "<u>%s<div class=tips><table class=det>",
					              mod->name);
					for (j = 0; j < mod->stats_count; ++j) {
						chunk_appendf(out,
						              "<tr><th>%s</th><td>%s</td></tr>",
						              mod->stats[j].desc, field_to_html_str(&stats[ST_F_TOTAL_FIELDS + i]));
						++i;
					}
					chunk_appendf(out, "</table></div></u>");
				} else {
					i += mod->stats_count;
				}

				chunk_appendf(out, "</td>");
			}
		}

		chunk_appendf(out, "</tr>");
	}
	else if (stats[ST_F_TYPE].u.u32 == STATS_TYPE_SO) {
		chunk_appendf(out, "<tr class=socket>");
		if (flags & STAT_ADMIN) {
			/* Column sub-heading for Enable or Disable server */
			chunk_appendf(out, "<td></td>");
		}

		chunk_appendf(out,
		              /* frontend name, listener name */
		              "<td class=ac><a name=\"%s/+%s\"></a>%s"
		              "<a class=lfsb href=\"#%s/+%s\">%s</a>"
		              "",
		              field_str(stats, ST_F_PXNAME), field_str(stats, ST_F_SVNAME),
		              (flags & STAT_SHLGNDS)?"<u>":"",
		              field_str(stats, ST_F_PXNAME), field_str(stats, ST_F_SVNAME), field_str(stats, ST_F_SVNAME));

		if (flags & STAT_SHLGNDS) {
			chunk_appendf(out, "<div class=tips>");

			if (isdigit((unsigned char)*field_str(stats, ST_F_ADDR)))
				chunk_appendf(out, "IPv4: %s, ", field_str(stats, ST_F_ADDR));
			else if (*field_str(stats, ST_F_ADDR) == '[')
				chunk_appendf(out, "IPv6: %s, ", field_str(stats, ST_F_ADDR));
			else if (*field_str(stats, ST_F_ADDR))
				chunk_appendf(out, "%s, ", field_str(stats, ST_F_ADDR));

			chunk_appendf(out, "proto=%s, ", field_str(stats, ST_F_PROTO));

			/* id */
			chunk_appendf(out, "id: %d</div>", stats[ST_F_SID].u.u32);
		}

		chunk_appendf(out,
			      /* queue */
		              "%s</td><td colspan=3></td>"
		              /* sessions rate: current, max, limit */
		              "<td colspan=3>&nbsp;</td>"
		              /* sessions: current, max, limit, total, lbtot, lastsess */
		              "<td>%s</td><td>%s</td><td>%s</td>"
		              "<td>%s</td><td>&nbsp;</td><td>&nbsp;</td>"
		              /* bytes: in, out */
		              "<td>%s</td><td>%s</td>"
		              "",
		              (flags & STAT_SHLGNDS)?"</u>":"",
		              U2H(stats[ST_F_SCUR].u.u32), U2H(stats[ST_F_SMAX].u.u32), U2H(stats[ST_F_SLIM].u.u32),
		              U2H(stats[ST_F_STOT].u.u64), U2H(stats[ST_F_BIN].u.u64), U2H(stats[ST_F_BOUT].u.u64));

		chunk_appendf(out,
		              /* denied: req, resp */
		              "<td>%s</td><td>%s</td>"
		              /* errors: request, connect, response */
		              "<td>%s</td><td></td><td></td>"
		              /* warnings: retries, redispatches */
		              "<td></td><td></td>"
		              /* server status: reflect listener status */
		              "<td class=ac>%s</td>"
		              /* rest of server: nothing */
		              "<td class=ac colspan=8></td>"
		              "",
		              U2H(stats[ST_F_DREQ].u.u64), U2H(stats[ST_F_DRESP].u.u64),
		              U2H(stats[ST_F_EREQ].u.u64),
		              field_str(stats, ST_F_STATUS));

		if (flags & STAT_SHMODULES) {
			list_for_each_entry(mod, &stats_module_list[STATS_DOMAIN_PROXY], list) {
				chunk_appendf(out, "<td>");

				if (stats_px_get_cap(mod->domain_flags) & STATS_PX_CAP_LI) {
					chunk_appendf(out,
					              "<u>%s<div class=tips><table class=det>",
					              mod->name);
					for (j = 0; j < mod->stats_count; ++j) {
						chunk_appendf(out,
						              "<tr><th>%s</th><td>%s</td></tr>",
						              mod->stats[j].desc, field_to_html_str(&stats[ST_F_TOTAL_FIELDS + i]));
						++i;
					}
					chunk_appendf(out, "</table></div></u>");
				} else {
					i += mod->stats_count;
				}

				chunk_appendf(out, "</td>");
			}
		}

		chunk_appendf(out, "</tr>");
	}
	else if (stats[ST_F_TYPE].u.u32 == STATS_TYPE_SV) {
		const char *style;

		/* determine the style to use depending on the server's state,
		 * its health and weight. There isn't a 1-to-1 mapping between
		 * state and styles for the cases where the server is (still)
		 * up. The reason is that we don't want to report nolb and
		 * drain with the same color.
		 */

		if (strcmp(field_str(stats, ST_F_STATUS), "DOWN") == 0 ||
		    strcmp(field_str(stats, ST_F_STATUS), "DOWN (agent)") == 0) {
			style = "down";
		}
		else if (strncmp(field_str(stats, ST_F_STATUS), "DOWN ", strlen("DOWN ")) == 0) {
			style = "going_up";
		}
		else if (strcmp(field_str(stats, ST_F_STATUS), "DRAIN") == 0) {
			style = "draining";
		}
		else if (strncmp(field_str(stats, ST_F_STATUS), "NOLB ", strlen("NOLB ")) == 0) {
			style = "going_down";
		}
		else if (strcmp(field_str(stats, ST_F_STATUS), "NOLB") == 0) {
			style = "nolb";
		}
		else if (strcmp(field_str(stats, ST_F_STATUS), "no check") == 0) {
			style = "no_check";
		}
		else if (!stats[ST_F_CHKFAIL].type ||
			 stats[ST_F_CHECK_HEALTH].u.u32 == stats[ST_F_CHECK_RISE].u.u32 + stats[ST_F_CHECK_FALL].u.u32 - 1) {
			/* no check or max health = UP */
			if (stats[ST_F_WEIGHT].u.u32)
				style = "up";
			else
				style = "draining";
		}
		else {
			style = "going_down";
		}

		if (strncmp(field_str(stats, ST_F_STATUS), "MAINT", 5) == 0)
			chunk_appendf(out, "<tr class=\"maintain\">");
		else
			chunk_appendf(out,
			              "<tr class=\"%s_%s\">",
			              (stats[ST_F_BCK].u.u32) ? "backup" : "active", style);


		if (flags & STAT_ADMIN)
			chunk_appendf(out,
			              "<td><input class='%s-checkbox' type=\"checkbox\" name=\"s\" value=\"%s\"></td>",
			              field_str(stats, ST_F_PXNAME),
			              field_str(stats, ST_F_SVNAME));

		chunk_appendf(out,
		              "<td class=ac><a name=\"%s/%s\"></a>%s"
		              "<a class=lfsb href=\"#%s/%s\">%s</a>"
		              "",
		              field_str(stats, ST_F_PXNAME), field_str(stats, ST_F_SVNAME),
		              (flags & STAT_SHLGNDS) ? "<u>" : "",
		              field_str(stats, ST_F_PXNAME), field_str(stats, ST_F_SVNAME), field_str(stats, ST_F_SVNAME));

		if (flags & STAT_SHLGNDS) {
			chunk_appendf(out, "<div class=tips>");

			if (isdigit((unsigned char)*field_str(stats, ST_F_ADDR)))
				chunk_appendf(out, "IPv4: %s, ", field_str(stats, ST_F_ADDR));
			else if (*field_str(stats, ST_F_ADDR) == '[')
				chunk_appendf(out, "IPv6: %s, ", field_str(stats, ST_F_ADDR));
			else if (*field_str(stats, ST_F_ADDR))
				chunk_appendf(out, "%s, ", field_str(stats, ST_F_ADDR));

			/* id */
			chunk_appendf(out, "id: %d, rid: %d", stats[ST_F_SID].u.u32, stats[ST_F_SRID].u.u32);

			/* cookie */
			if (stats[ST_F_COOKIE].type) {
				chunk_appendf(out, ", cookie: '");
				chunk_initstr(&src, field_str(stats, ST_F_COOKIE));
				chunk_htmlencode(out, &src);
				chunk_appendf(out, "'");
			}

			chunk_appendf(out, "</div>");
		}

		chunk_appendf(out,
		              /* queue : current, max, limit */
		              "%s</td><td>%s</td><td>%s</td><td>%s</td>"
		              /* sessions rate : current, max, limit */
		              "<td>%s</td><td>%s</td><td></td>"
		              "",
		              (flags & STAT_SHLGNDS) ? "</u>" : "",
		              U2H(stats[ST_F_QCUR].u.u32), U2H(stats[ST_F_QMAX].u.u32), LIM2A(stats[ST_F_QLIMIT].u.u32, "-"),
		              U2H(stats[ST_F_RATE].u.u32), U2H(stats[ST_F_RATE_MAX].u.u32));

		chunk_appendf(out,
		              /* sessions: current, max, limit, total */
		              "<td><u>%s<div class=tips>"
			        "<table class=det>"
		                "<tr><th>Current active connections:</th><td>%s</td></tr>"
		                "<tr><th>Current used connections:</th><td>%s</td></tr>"
		                "<tr><th>Current idle connections:</th><td>%s</td></tr>"
		                "<tr><th>- unsafe:</th><td>%s</td></tr>"
		                "<tr><th>- safe:</th><td>%s</td></tr>"
		                "<tr><th>Estimated need of connections:</th><td>%s</td></tr>"
		                "<tr><th>Active connections limit:</th><td>%s</td></tr>"
		                "<tr><th>Idle connections limit:</th><td>%s</td></tr>"
			        "</table></div></u>"
			      "</td><td>%s</td><td>%s</td>"
		              "<td><u>%s<div class=tips><table class=det>"
		              "<tr><th>Cum. sessions:</th><td>%s</td></tr>"
		              "",
		              U2H(stats[ST_F_SCUR].u.u32),
			      U2H(stats[ST_F_SCUR].u.u32),
			      U2H(stats[ST_F_USED_CONN_CUR].u.u32),
			      U2H(stats[ST_F_SRV_ICUR].u.u32),
			      U2H(stats[ST_F_IDLE_CONN_CUR].u.u32),
			      U2H(stats[ST_F_SAFE_CONN_CUR].u.u32),
			      U2H(stats[ST_F_NEED_CONN_EST].u.u32),

			        LIM2A(stats[ST_F_SLIM].u.u32, "-"),
		                stats[ST_F_SRV_ILIM].type ? U2H(stats[ST_F_SRV_ILIM].u.u32) : "-",
			      U2H(stats[ST_F_SMAX].u.u32), LIM2A(stats[ST_F_SLIM].u.u32, "-"),
		              U2H(stats[ST_F_STOT].u.u64),
		              U2H(stats[ST_F_STOT].u.u64));

		/* http response (via hover): 1xx, 2xx, 3xx, 4xx, 5xx, other */
		if (strcmp(field_str(stats, ST_F_MODE), "http") == 0) {
			chunk_appendf(out,
			              "<tr><th>New connections:</th><td>%s</td></tr>"
			              "<tr><th>Reused connections:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>Cum. HTTP requests:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 1xx responses:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>- HTTP 2xx responses:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>- HTTP 3xx responses:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>- HTTP 4xx responses:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>- HTTP 5xx responses:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>- other responses:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>Failed hdr rewrites:</th><td>%s</td></tr>"
			              "<tr><th>Internal error:</th><td>%s</td></tr>"
			              "",
			              U2H(stats[ST_F_CONNECT].u.u64),
			              U2H(stats[ST_F_REUSE].u.u64),
			              (stats[ST_F_CONNECT].u.u64 + stats[ST_F_REUSE].u.u64) ?
			              (int)(100 * stats[ST_F_REUSE].u.u64 / (stats[ST_F_CONNECT].u.u64 + stats[ST_F_REUSE].u.u64)) : 0,
			              U2H(stats[ST_F_REQ_TOT].u.u64),
			              U2H(stats[ST_F_HRSP_1XX].u.u64), stats[ST_F_REQ_TOT].u.u64 ?
			              (int)(100 * stats[ST_F_HRSP_1XX].u.u64 / stats[ST_F_REQ_TOT].u.u64) : 0,
			              U2H(stats[ST_F_HRSP_2XX].u.u64), stats[ST_F_REQ_TOT].u.u64 ?
			              (int)(100 * stats[ST_F_HRSP_2XX].u.u64 / stats[ST_F_REQ_TOT].u.u64) : 0,
			              U2H(stats[ST_F_HRSP_3XX].u.u64), stats[ST_F_REQ_TOT].u.u64 ?
			              (int)(100 * stats[ST_F_HRSP_3XX].u.u64 / stats[ST_F_REQ_TOT].u.u64) : 0,
			              U2H(stats[ST_F_HRSP_4XX].u.u64), stats[ST_F_REQ_TOT].u.u64 ?
			              (int)(100 * stats[ST_F_HRSP_4XX].u.u64 / stats[ST_F_REQ_TOT].u.u64) : 0,
			              U2H(stats[ST_F_HRSP_5XX].u.u64), stats[ST_F_REQ_TOT].u.u64 ?
			              (int)(100 * stats[ST_F_HRSP_5XX].u.u64 / stats[ST_F_REQ_TOT].u.u64) : 0,
			              U2H(stats[ST_F_HRSP_OTHER].u.u64), stats[ST_F_REQ_TOT].u.u64 ?
			              (int)(100 * stats[ST_F_HRSP_OTHER].u.u64 / stats[ST_F_REQ_TOT].u.u64) : 0,
			              U2H(stats[ST_F_WREW].u.u64),
			              U2H(stats[ST_F_EINT].u.u64));
		}

		chunk_appendf(out, "<tr><th colspan=3>Max / Avg over last 1024 success. conn.</th></tr>");
		chunk_appendf(out, "<tr><th>- Queue time:</th><td>%s / %s</td><td>ms</td></tr>",
			      U2H(stats[ST_F_QT_MAX].u.u32), U2H(stats[ST_F_QTIME].u.u32));
		chunk_appendf(out, "<tr><th>- Connect time:</th><td>%s / %s</td><td>ms</td></tr>",
			      U2H(stats[ST_F_CT_MAX].u.u32), U2H(stats[ST_F_CTIME].u.u32));
		if (strcmp(field_str(stats, ST_F_MODE), "http") == 0)
			chunk_appendf(out, "<tr><th>- Responses time:</th><td>%s / %s</td><td>ms</td></tr>",
				      U2H(stats[ST_F_RT_MAX].u.u32), U2H(stats[ST_F_RTIME].u.u32));
		chunk_appendf(out, "<tr><th>- Total time:</th><td>%s / %s</td><td>ms</td></tr>",
			      U2H(stats[ST_F_TT_MAX].u.u32), U2H(stats[ST_F_TTIME].u.u32));

		chunk_appendf(out,
		              "</table></div></u></td>"
		              /* sessions: lbtot, last */
		              "<td>%s</td><td>%s</td>",
		              U2H(stats[ST_F_LBTOT].u.u64),
		              human_time(stats[ST_F_LASTSESS].u.s32, 1));

		chunk_appendf(out,
		              /* bytes : in, out */
		              "<td>%s</td><td>%s</td>"
		              /* denied: req, resp */
		              "<td></td><td>%s</td>"
		              /* errors : request, connect */
		              "<td></td><td>%s</td>"
		              /* errors : response */
		              "<td><u>%s<div class=tips>Connection resets during transfers: %lld client, %lld server</div></u></td>"
		              /* warnings: retries, redispatches */
		              "<td>%lld</td><td>%lld</td>"
		              "",
		              U2H(stats[ST_F_BIN].u.u64), U2H(stats[ST_F_BOUT].u.u64),
		              U2H(stats[ST_F_DRESP].u.u64),
		              U2H(stats[ST_F_ECON].u.u64),
		              U2H(stats[ST_F_ERESP].u.u64),
		              (long long)stats[ST_F_CLI_ABRT].u.u64,
		              (long long)stats[ST_F_SRV_ABRT].u.u64,
		              (long long)stats[ST_F_WRETR].u.u64,
			      (long long)stats[ST_F_WREDIS].u.u64);

		/* status, last change */
		chunk_appendf(out, "<td class=ac>");

		/* FIXME!!!!
		 *   LASTCHG should contain the last change for *this* server and must be computed
		 * properly above, as was done below, ie: this server if maint, otherwise ref server
		 * if tracking. Note that ref is either local or remote depending on tracking.
		 */


		if (strncmp(field_str(stats, ST_F_STATUS), "MAINT", 5) == 0) {
			chunk_appendf(out, "%s MAINT", human_time(stats[ST_F_LASTCHG].u.u32, 1));
		}
		else if (strcmp(field_str(stats, ST_F_STATUS), "no check") == 0) {
			chunk_strcat(out, "<i>no check</i>");
		}
		else {
			chunk_appendf(out, "%s %s", human_time(stats[ST_F_LASTCHG].u.u32, 1), field_str(stats, ST_F_STATUS));
			if (strncmp(field_str(stats, ST_F_STATUS), "DOWN", 4) == 0) {
				if (stats[ST_F_CHECK_HEALTH].u.u32)
					chunk_strcat(out, " &uarr;");
			}
			else if (stats[ST_F_CHECK_HEALTH].u.u32 < stats[ST_F_CHECK_RISE].u.u32 + stats[ST_F_CHECK_FALL].u.u32 - 1)
				chunk_strcat(out, " &darr;");
		}

		if (strncmp(field_str(stats, ST_F_STATUS), "DOWN", 4) == 0 &&
		    stats[ST_F_AGENT_STATUS].type && !stats[ST_F_AGENT_HEALTH].u.u32) {
			chunk_appendf(out,
			              "</td><td class=ac><u> %s",
			              field_str(stats, ST_F_AGENT_STATUS));

			if (stats[ST_F_AGENT_CODE].type)
				chunk_appendf(out, "/%d", stats[ST_F_AGENT_CODE].u.u32);

			if (stats[ST_F_AGENT_DURATION].type)
				chunk_appendf(out, " in %lums", (long)stats[ST_F_AGENT_DURATION].u.u64);

			chunk_appendf(out, "<div class=tips>%s", field_str(stats, ST_F_AGENT_DESC));

			if (*field_str(stats, ST_F_LAST_AGT)) {
				chunk_appendf(out, ": ");
				chunk_initstr(&src, field_str(stats, ST_F_LAST_AGT));
				chunk_htmlencode(out, &src);
			}
			chunk_appendf(out, "</div></u>");
		}
		else if (stats[ST_F_CHECK_STATUS].type) {
			chunk_appendf(out,
			              "</td><td class=ac><u> %s",
			              field_str(stats, ST_F_CHECK_STATUS));

			if (stats[ST_F_CHECK_CODE].type)
				chunk_appendf(out, "/%d", stats[ST_F_CHECK_CODE].u.u32);

			if (stats[ST_F_CHECK_DURATION].type)
				chunk_appendf(out, " in %lums", (long)stats[ST_F_CHECK_DURATION].u.u64);

			chunk_appendf(out, "<div class=tips>%s", field_str(stats, ST_F_CHECK_DESC));

			if (*field_str(stats, ST_F_LAST_CHK)) {
				chunk_appendf(out, ": ");
				chunk_initstr(&src, field_str(stats, ST_F_LAST_CHK));
				chunk_htmlencode(out, &src);
			}
			chunk_appendf(out, "</div></u>");
		}
		else
			chunk_appendf(out, "</td><td>");

		chunk_appendf(out,
		              /* weight / uweight */
		              "</td><td class=ac>%d/%d</td>"
		              /* act, bck */
		              "<td class=ac>%s</td><td class=ac>%s</td>"
		              "",
		              stats[ST_F_WEIGHT].u.u32, stats[ST_F_UWEIGHT].u.u32,
		              stats[ST_F_BCK].u.u32 ? "-" : "Y",
		              stats[ST_F_BCK].u.u32 ? "Y" : "-");

		/* check failures: unique, fatal, down time */
		if (strcmp(field_str(stats, ST_F_STATUS), "MAINT (resolution)") == 0) {
			chunk_appendf(out, "<td class=ac colspan=3>resolution</td>");
		}
		else if (stats[ST_F_CHKFAIL].type) {
			chunk_appendf(out, "<td><u>%lld", (long long)stats[ST_F_CHKFAIL].u.u64);

			if (stats[ST_F_HANAFAIL].type)
				chunk_appendf(out, "/%lld", (long long)stats[ST_F_HANAFAIL].u.u64);

			chunk_appendf(out,
			              "<div class=tips>Failed Health Checks%s</div></u></td>"
			              "<td>%lld</td><td>%s</td>"
			              "",
			              stats[ST_F_HANAFAIL].type ? "/Health Analyses" : "",
			              (long long)stats[ST_F_CHKDOWN].u.u64, human_time(stats[ST_F_DOWNTIME].u.u32, 1));
		}
		else if (strcmp(field_str(stats, ST_F_STATUS), "MAINT") != 0 && field_format(stats, ST_F_TRACKED) == FF_STR) {
			/* tracking a server (hence inherited maint would appear as "MAINT (via...)" */
			chunk_appendf(out,
			              "<td class=ac colspan=3><a class=lfsb href=\"#%s\">via %s</a></td>",
			              field_str(stats, ST_F_TRACKED), field_str(stats, ST_F_TRACKED));
		}
		else
			chunk_appendf(out, "<td colspan=3></td>");

		/* throttle */
		if (stats[ST_F_THROTTLE].type)
			chunk_appendf(out, "<td class=ac>%d %%</td>\n", stats[ST_F_THROTTLE].u.u32);
		else
			chunk_appendf(out, "<td class=ac>-</td>");

		if (flags & STAT_SHMODULES) {
			list_for_each_entry(mod, &stats_module_list[STATS_DOMAIN_PROXY], list) {
				chunk_appendf(out, "<td>");

				if (stats_px_get_cap(mod->domain_flags) & STATS_PX_CAP_SRV) {
					chunk_appendf(out,
					              "<u>%s<div class=tips><table class=det>",
					              mod->name);
					for (j = 0; j < mod->stats_count; ++j) {
						chunk_appendf(out,
						              "<tr><th>%s</th><td>%s</td></tr>",
						              mod->stats[j].desc, field_to_html_str(&stats[ST_F_TOTAL_FIELDS + i]));
						++i;
					}
					chunk_appendf(out, "</table></div></u>");
				} else {
					i += mod->stats_count;
				}

				chunk_appendf(out, "</td>");
			}
		}

		chunk_appendf(out, "</tr>\n");
	}
	else if (stats[ST_F_TYPE].u.u32 == STATS_TYPE_BE) {
		chunk_appendf(out, "<tr class=\"backend\">");
		if (flags & STAT_ADMIN) {
			/* Column sub-heading for Enable or Disable server */
			chunk_appendf(out, "<td></td>");
		}
		chunk_appendf(out,
		              "<td class=ac>"
		              /* name */
		              "%s<a name=\"%s/Backend\"></a>"
		              "<a class=lfsb href=\"#%s/Backend\">Backend</a>"
		              "",
		              (flags & STAT_SHLGNDS)?"<u>":"",
		              field_str(stats, ST_F_PXNAME), field_str(stats, ST_F_PXNAME));

		if (flags & STAT_SHLGNDS) {
			/* balancing */
			chunk_appendf(out, "<div class=tips>balancing: %s",
			              field_str(stats, ST_F_ALGO));

			/* cookie */
			if (stats[ST_F_COOKIE].type) {
				chunk_appendf(out, ", cookie: '");
				chunk_initstr(&src, field_str(stats, ST_F_COOKIE));
				chunk_htmlencode(out, &src);
				chunk_appendf(out, "'");
			}
			chunk_appendf(out, "</div>");
		}

		chunk_appendf(out,
		              "%s</td>"
		              /* queue : current, max */
		              "<td>%s</td><td>%s</td><td></td>"
		              /* sessions rate : current, max, limit */
		              "<td>%s</td><td>%s</td><td></td>"
		              "",
		              (flags & STAT_SHLGNDS)?"</u>":"",
		              U2H(stats[ST_F_QCUR].u.u32), U2H(stats[ST_F_QMAX].u.u32),
		              U2H(stats[ST_F_RATE].u.u32), U2H(stats[ST_F_RATE_MAX].u.u32));

		chunk_appendf(out,
		              /* sessions: current, max, limit, total */
		              "<td>%s</td><td>%s</td><td>%s</td>"
		              "<td><u>%s<div class=tips><table class=det>"
		              "<tr><th>Cum. sessions:</th><td>%s</td></tr>"
		              "",
		              U2H(stats[ST_F_SCUR].u.u32), U2H(stats[ST_F_SMAX].u.u32), U2H(stats[ST_F_SLIM].u.u32),
		              U2H(stats[ST_F_STOT].u.u64),
		              U2H(stats[ST_F_STOT].u.u64));

		/* http response (via hover): 1xx, 2xx, 3xx, 4xx, 5xx, other */
		if (strcmp(field_str(stats, ST_F_MODE), "http") == 0) {
			chunk_appendf(out,
			              "<tr><th>New connections:</th><td>%s</td></tr>"
			              "<tr><th>Reused connections:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>Cum. HTTP requests:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 1xx responses:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 2xx responses:</th><td>%s</td></tr>"
			              "<tr><th>&nbsp;&nbsp;Compressed 2xx:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>- HTTP 3xx responses:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 4xx responses:</th><td>%s</td></tr>"
			              "<tr><th>- HTTP 5xx responses:</th><td>%s</td></tr>"
			              "<tr><th>- other responses:</th><td>%s</td></tr>"
			              "<tr><th>Cache lookups:</th><td>%s</td></tr>"
			              "<tr><th>Cache hits:</th><td>%s</td><td>(%d%%)</td></tr>"
			              "<tr><th>Failed hdr rewrites:</th><td>%s</td></tr>"
			              "<tr><th>Internal errors:</th><td>%s</td></tr>"
				      "",
			              U2H(stats[ST_F_CONNECT].u.u64),
			              U2H(stats[ST_F_REUSE].u.u64),
			              (stats[ST_F_CONNECT].u.u64 + stats[ST_F_REUSE].u.u64) ?
			              (int)(100 * stats[ST_F_REUSE].u.u64 / (stats[ST_F_CONNECT].u.u64 + stats[ST_F_REUSE].u.u64)) : 0,
			              U2H(stats[ST_F_REQ_TOT].u.u64),
			              U2H(stats[ST_F_HRSP_1XX].u.u64),
			              U2H(stats[ST_F_HRSP_2XX].u.u64),
			              U2H(stats[ST_F_COMP_RSP].u.u64),
			              stats[ST_F_HRSP_2XX].u.u64 ?
			              (int)(100 * stats[ST_F_COMP_RSP].u.u64 / stats[ST_F_HRSP_2XX].u.u64) : 0,
			              U2H(stats[ST_F_HRSP_3XX].u.u64),
			              U2H(stats[ST_F_HRSP_4XX].u.u64),
			              U2H(stats[ST_F_HRSP_5XX].u.u64),
			              U2H(stats[ST_F_HRSP_OTHER].u.u64),
			              U2H(stats[ST_F_CACHE_LOOKUPS].u.u64),
			              U2H(stats[ST_F_CACHE_HITS].u.u64),
			              stats[ST_F_CACHE_LOOKUPS].u.u64 ?
			              (int)(100 * stats[ST_F_CACHE_HITS].u.u64 / stats[ST_F_CACHE_LOOKUPS].u.u64) : 0,
			              U2H(stats[ST_F_WREW].u.u64),
			              U2H(stats[ST_F_EINT].u.u64));
		}

		chunk_appendf(out, "<tr><th colspan=3>Max / Avg over last 1024 success. conn.</th></tr>");
		chunk_appendf(out, "<tr><th>- Queue time:</th><td>%s / %s</td><td>ms</td></tr>",
			      U2H(stats[ST_F_QT_MAX].u.u32), U2H(stats[ST_F_QTIME].u.u32));
		chunk_appendf(out, "<tr><th>- Connect time:</th><td>%s / %s</td><td>ms</td></tr>",
			      U2H(stats[ST_F_CT_MAX].u.u32), U2H(stats[ST_F_CTIME].u.u32));
		if (strcmp(field_str(stats, ST_F_MODE), "http") == 0)
			chunk_appendf(out, "<tr><th>- Responses time:</th><td>%s / %s</td><td>ms</td></tr>",
				      U2H(stats[ST_F_RT_MAX].u.u32), U2H(stats[ST_F_RTIME].u.u32));
		chunk_appendf(out, "<tr><th>- Total time:</th><td>%s / %s</td><td>ms</td></tr>",
			      U2H(stats[ST_F_TT_MAX].u.u32), U2H(stats[ST_F_TTIME].u.u32));

		chunk_appendf(out,
		              "</table></div></u></td>"
		              /* sessions: lbtot, last */
		              "<td>%s</td><td>%s</td>"
		              /* bytes: in */
		              "<td>%s</td>"
		              "",
		              U2H(stats[ST_F_LBTOT].u.u64),
		              human_time(stats[ST_F_LASTSESS].u.s32, 1),
		              U2H(stats[ST_F_BIN].u.u64));

		chunk_appendf(out,
			      /* bytes:out + compression stats (via hover): comp_in, comp_out, comp_byp */
		              "<td>%s%s<div class=tips><table class=det>"
			      "<tr><th>Response bytes in:</th><td>%s</td></tr>"
			      "<tr><th>Compression in:</th><td>%s</td></tr>"
			      "<tr><th>Compression out:</th><td>%s</td><td>(%d%%)</td></tr>"
			      "<tr><th>Compression bypass:</th><td>%s</td></tr>"
			      "<tr><th>Total bytes saved:</th><td>%s</td><td>(%d%%)</td></tr>"
			      "</table></div>%s</td>",
		              (stats[ST_F_COMP_IN].u.u64 || stats[ST_F_COMP_BYP].u.u64) ? "<u>":"",
		              U2H(stats[ST_F_BOUT].u.u64),
		              U2H(stats[ST_F_BOUT].u.u64),
		              U2H(stats[ST_F_COMP_IN].u.u64),
			      U2H(stats[ST_F_COMP_OUT].u.u64),
			      stats[ST_F_COMP_IN].u.u64 ? (int)(stats[ST_F_COMP_OUT].u.u64 * 100 / stats[ST_F_COMP_IN].u.u64) : 0,
			      U2H(stats[ST_F_COMP_BYP].u.u64),
			      U2H(stats[ST_F_COMP_IN].u.u64 - stats[ST_F_COMP_OUT].u.u64),
			      stats[ST_F_BOUT].u.u64 ? (int)((stats[ST_F_COMP_IN].u.u64 - stats[ST_F_COMP_OUT].u.u64) * 100 / stats[ST_F_BOUT].u.u64) : 0,
		              (stats[ST_F_COMP_IN].u.u64 || stats[ST_F_COMP_BYP].u.u64) ? "</u>":"");

		chunk_appendf(out,
		              /* denied: req, resp */
		              "<td>%s</td><td>%s</td>"
		              /* errors : request, connect */
		              "<td></td><td>%s</td>"
		              /* errors : response */
		              "<td><u>%s<div class=tips>Connection resets during transfers: %lld client, %lld server</div></u></td>"
		              /* warnings: retries, redispatches */
		              "<td>%lld</td><td>%lld</td>"
		              /* backend status: reflect backend status (up/down): we display UP
		               * if the backend has known working servers or if it has no server at
		               * all (eg: for stats). Then we display the total weight, number of
		               * active and backups. */
		              "<td class=ac>%s %s</td><td class=ac>&nbsp;</td><td class=ac>%d/%d</td>"
		              "<td class=ac>%d</td><td class=ac>%d</td>"
		              "",
		              U2H(stats[ST_F_DREQ].u.u64), U2H(stats[ST_F_DRESP].u.u64),
		              U2H(stats[ST_F_ECON].u.u64),
		              U2H(stats[ST_F_ERESP].u.u64),
		              (long long)stats[ST_F_CLI_ABRT].u.u64,
		              (long long)stats[ST_F_SRV_ABRT].u.u64,
		              (long long)stats[ST_F_WRETR].u.u64, (long long)stats[ST_F_WREDIS].u.u64,
		              human_time(stats[ST_F_LASTCHG].u.u32, 1),
		              strcmp(field_str(stats, ST_F_STATUS), "DOWN") ? field_str(stats, ST_F_STATUS) : "<font color=\"red\"><b>DOWN</b></font>",
		              stats[ST_F_WEIGHT].u.u32, stats[ST_F_UWEIGHT].u.u32,
		              stats[ST_F_ACT].u.u32, stats[ST_F_BCK].u.u32);

		chunk_appendf(out,
		              /* rest of backend: nothing, down transitions, total downtime, throttle */
		              "<td class=ac>&nbsp;</td><td>%d</td>"
		              "<td>%s</td>"
		              "<td></td>",
		              stats[ST_F_CHKDOWN].u.u32,
		              stats[ST_F_DOWNTIME].type ? human_time(stats[ST_F_DOWNTIME].u.u32, 1) : "&nbsp;");

		if (flags & STAT_SHMODULES) {
			list_for_each_entry(mod, &stats_module_list[STATS_DOMAIN_PROXY], list) {
				chunk_appendf(out, "<td>");

				if (stats_px_get_cap(mod->domain_flags) & STATS_PX_CAP_BE) {
					chunk_appendf(out,
					              "<u>%s<div class=tips><table class=det>",
					              mod->name);
					for (j = 0; j < mod->stats_count; ++j) {
						chunk_appendf(out,
						              "<tr><th>%s</th><td>%s</td></tr>",
						              mod->stats[j].desc, field_to_html_str(&stats[ST_F_TOTAL_FIELDS + i]));
						++i;
					}
					chunk_appendf(out, "</table></div></u>");
				} else {
					i += mod->stats_count;
				}

				chunk_appendf(out, "</td>");
			}
		}

		chunk_appendf(out, "</tr>");
	}

	return 1;
}

int stats_dump_one_line(const struct field *stats, size_t stats_count,
                        struct appctx *appctx)
{
	struct show_stat_ctx *ctx = appctx->svcctx;
	int ret;

	if (ctx->flags & STAT_FMT_HTML)
		ret = stats_dump_fields_html(&trash_chunk, stats, ctx);
	else if (ctx->flags & STAT_FMT_TYPED)
		ret = stats_dump_fields_typed(&trash_chunk, stats, stats_count, ctx);
	else if (ctx->flags & STAT_FMT_JSON)
		ret = stats_dump_fields_json(&trash_chunk, stats, stats_count, ctx);
	else
		ret = stats_dump_fields_csv(&trash_chunk, stats, stats_count, ctx);

	return ret;
}

/* Fill <stats> with the frontend statistics. <stats> is preallocated array of
 * length <len>. If <selected_field> is != NULL, only fill this one. The length
 * of the array must be at least ST_F_TOTAL_FIELDS. If this length is less than
 * this value, or if the selected field is not implemented for frontends, the
 * function returns 0, otherwise, it returns 1.
 */
int stats_fill_fe_stats(struct proxy *px, struct field *stats, int len,
			enum stat_field *selected_field)
{
	enum stat_field current_field = (selected_field != NULL ? *selected_field : 0);

	if (len < ST_F_TOTAL_FIELDS)
		return 0;

	for (; current_field < ST_F_TOTAL_FIELDS; current_field++) {
		struct field metric = { 0 };

		switch (current_field) {
			case ST_F_PXNAME:
				metric = mkf_str(FO_KEY|FN_NAME|FS_SERVICE, px->id);
				break;
			case ST_F_SVNAME:
				metric = mkf_str(FO_KEY|FN_NAME|FS_SERVICE, "FRONTEND");
				break;
			case ST_F_MODE:
				metric = mkf_str(FO_CONFIG|FS_SERVICE, proxy_mode_str(px->mode));
				break;
			case ST_F_SCUR:
				metric = mkf_u32(0, px->feconn);
				break;
			case ST_F_SMAX:
				metric = mkf_u32(FN_MAX, px->fe_counters.conn_max);
				break;
			case ST_F_SLIM:
				metric = mkf_u32(FO_CONFIG|FN_LIMIT, px->maxconn);
				break;
			case ST_F_STOT:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.cum_sess);
				break;
			case ST_F_BIN:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.bytes_in);
				break;
			case ST_F_BOUT:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.bytes_out);
				break;
			case ST_F_DREQ:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.denied_req);
				break;
			case ST_F_DRESP:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.denied_resp);
				break;
			case ST_F_EREQ:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.failed_req);
				break;
			case ST_F_DCON:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.denied_conn);
				break;
			case ST_F_DSES:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.denied_sess);
				break;
			case ST_F_STATUS: {
				const char *state;

				if (px->flags & (PR_FL_DISABLED|PR_FL_STOPPED))
					state = "STOP";
				else if (px->flags & PR_FL_PAUSED)
					state = "PAUSED";
				else
					state = "OPEN";
				metric = mkf_str(FO_STATUS, state);
				break;
			}
			case ST_F_PID:
				metric = mkf_u32(FO_KEY, 1);
				break;
			case ST_F_IID:
				metric = mkf_u32(FO_KEY|FS_SERVICE, px->uuid);
				break;
			case ST_F_SID:
				metric = mkf_u32(FO_KEY|FS_SERVICE, 0);
				break;
			case ST_F_TYPE:
				metric = mkf_u32(FO_CONFIG|FS_SERVICE, STATS_TYPE_FE);
				break;
			case ST_F_RATE:
				metric = mkf_u32(FN_RATE, read_freq_ctr(&px->fe_sess_per_sec));
				break;
			case ST_F_RATE_LIM:
				metric = mkf_u32(FO_CONFIG|FN_LIMIT, px->fe_sps_lim);
				break;
			case ST_F_RATE_MAX:
				metric = mkf_u32(FN_MAX, px->fe_counters.sps_max);
				break;
			case ST_F_WREW:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.failed_rewrites);
				break;
			case ST_F_EINT:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.internal_errors);
				break;
			case ST_F_HRSP_1XX:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->fe_counters.p.http.rsp[1]);
				break;
			case ST_F_HRSP_2XX:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->fe_counters.p.http.rsp[2]);
				break;
			case ST_F_HRSP_3XX:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->fe_counters.p.http.rsp[3]);
				break;
			case ST_F_HRSP_4XX:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->fe_counters.p.http.rsp[4]);
				break;
			case ST_F_HRSP_5XX:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->fe_counters.p.http.rsp[5]);
				break;
			case ST_F_HRSP_OTHER:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->fe_counters.p.http.rsp[0]);
				break;
			case ST_F_INTERCEPTED:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->fe_counters.intercepted_req);
				break;
			case ST_F_CACHE_LOOKUPS:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->fe_counters.p.http.cache_lookups);
				break;
			case ST_F_CACHE_HITS:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->fe_counters.p.http.cache_hits);
				break;
			case ST_F_REQ_RATE:
				metric = mkf_u32(FN_RATE, read_freq_ctr(&px->fe_req_per_sec));
				break;
			case ST_F_REQ_RATE_MAX:
				metric = mkf_u32(FN_MAX, px->fe_counters.p.http.rps_max);
				break;
			case ST_F_REQ_TOT: {
				int i;
				uint64_t total_req;
				size_t nb_reqs =
					sizeof(px->fe_counters.p.http.cum_req) / sizeof(*px->fe_counters.p.http.cum_req);

				total_req = 0;
				for (i = 0; i < nb_reqs; i++)
					total_req += px->fe_counters.p.http.cum_req[i];
				metric = mkf_u64(FN_COUNTER, total_req);
				break;
			}
			case ST_F_COMP_IN:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.comp_in[COMP_DIR_RES]);
				break;
			case ST_F_COMP_OUT:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.comp_out[COMP_DIR_RES]);
				break;
			case ST_F_COMP_BYP:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.comp_byp[COMP_DIR_RES]);
				break;
			case ST_F_COMP_RSP:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.p.http.comp_rsp);
				break;
			case ST_F_CONN_RATE:
				metric = mkf_u32(FN_RATE, read_freq_ctr(&px->fe_conn_per_sec));
				break;
			case ST_F_CONN_RATE_MAX:
				metric = mkf_u32(FN_MAX, px->fe_counters.cps_max);
				break;
			case ST_F_CONN_TOT:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.cum_conn);
				break;
			case ST_F_SESS_OTHER: {
				int i;
				uint64_t total_sess;
				size_t nb_sess =
					sizeof(px->fe_counters.cum_sess_ver) / sizeof(*px->fe_counters.cum_sess_ver);

				total_sess = px->fe_counters.cum_sess;
				for (i = 0; i < nb_sess; i++)
					total_sess -= px->fe_counters.cum_sess_ver[i];
				total_sess = (int64_t)total_sess < 0 ? 0 : total_sess;
				metric = mkf_u64(FN_COUNTER, total_sess);
				break;
			}
			case ST_F_H1SESS:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.cum_sess_ver[0]);
				break;
			case ST_F_H2SESS:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.cum_sess_ver[1]);
				break;
			case ST_F_H3SESS:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.cum_sess_ver[2]);
				break;
			case ST_F_REQ_OTHER:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.p.http.cum_req[0]);
				break;
			case ST_F_H1REQ:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.p.http.cum_req[1]);
				break;
			case ST_F_H2REQ:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.p.http.cum_req[2]);
				break;
			case ST_F_H3REQ:
				metric = mkf_u64(FN_COUNTER, px->fe_counters.p.http.cum_req[3]);
				break;
			default:
				/* not used for frontends. If a specific metric
				 * is requested, return an error. Otherwise continue.
				 */
				if (selected_field != NULL)
					return 0;
				continue;
		}
		stats[current_field] = metric;
		if (selected_field != NULL)
			break;
	}
	return 1;
}

/* Dumps a frontend's line to the local trash buffer for the current proxy <px>
 * and uses the state from stream connector <sc>. The caller is responsible for
 * clearing the local trash buffer if needed. Returns non-zero if it emits
 * anything, zero otherwise.
 */
static int stats_dump_fe_stats(struct stconn *sc, struct proxy *px)
{
	struct appctx *appctx = __sc_appctx(sc);
	struct show_stat_ctx *ctx = appctx->svcctx;
	struct field *stats = stat_l[STATS_DOMAIN_PROXY];
	struct stats_module *mod;
	size_t stats_count = ST_F_TOTAL_FIELDS;

	if (!(px->cap & PR_CAP_FE))
		return 0;

	if ((ctx->flags & STAT_BOUND) && !(ctx->type & (1 << STATS_TYPE_FE)))
		return 0;

	memset(stats, 0, sizeof(struct field) * stat_count[STATS_DOMAIN_PROXY]);

	if (!stats_fill_fe_stats(px, stats, ST_F_TOTAL_FIELDS, NULL))
		return 0;

	list_for_each_entry(mod, &stats_module_list[STATS_DOMAIN_PROXY], list) {
		void *counters;

		if (!(stats_px_get_cap(mod->domain_flags) & STATS_PX_CAP_FE)) {
			stats_count += mod->stats_count;
			continue;
		}

		counters = EXTRA_COUNTERS_GET(px->extra_counters_fe, mod);
		mod->fill_stats(counters, stats + stats_count);
		stats_count += mod->stats_count;
	}

	return stats_dump_one_line(stats, stats_count, appctx);
}

/* Fill <stats> with the listener statistics. <stats> is preallocated array of
 * length <len>. The length of the array must be at least ST_F_TOTAL_FIELDS. If
 * this length is less then this value, the function returns 0, otherwise, it
 * returns 1.  If selected_field is != NULL, only fill this one. <flags> can
 * take the value STAT_SHLGNDS.
 */
int stats_fill_li_stats(struct proxy *px, struct listener *l, int flags,
                        struct field *stats, int len, enum stat_field *selected_field)
{
	enum stat_field current_field = (selected_field != NULL ? *selected_field : 0);
	struct buffer *out = get_trash_chunk();

	if (len < ST_F_TOTAL_FIELDS)
		return 0;

	if (!l->counters)
		return 0;

	chunk_reset(out);

	for (; current_field < ST_F_TOTAL_FIELDS; current_field++) {
		struct field metric = { 0 };

		switch (current_field) {
			case ST_F_PXNAME:
				metric = mkf_str(FO_KEY|FN_NAME|FS_SERVICE, px->id);
				break;
			case ST_F_SVNAME:
				metric = mkf_str(FO_KEY|FN_NAME|FS_SERVICE, l->name);
				break;
			case ST_F_MODE:
				metric = mkf_str(FO_CONFIG|FS_SERVICE, proxy_mode_str(px->mode));
				break;
			case ST_F_SCUR:
				metric = mkf_u32(0, l->nbconn);
				break;
			case ST_F_SMAX:
				metric = mkf_u32(FN_MAX, l->counters->conn_max);
				break;
			case ST_F_SLIM:
				metric = mkf_u32(FO_CONFIG|FN_LIMIT, l->bind_conf->maxconn);
				break;
			case ST_F_STOT:
				metric = mkf_u64(FN_COUNTER, l->counters->cum_conn);
				break;
			case ST_F_BIN:
				metric = mkf_u64(FN_COUNTER, l->counters->bytes_in);
				break;
			case ST_F_BOUT:
				metric = mkf_u64(FN_COUNTER, l->counters->bytes_out);
				break;
			case ST_F_DREQ:
				metric = mkf_u64(FN_COUNTER, l->counters->denied_req);
				break;
			case ST_F_DRESP:
				metric = mkf_u64(FN_COUNTER, l->counters->denied_resp);
				break;
			case ST_F_EREQ:
				metric = mkf_u64(FN_COUNTER, l->counters->failed_req);
				break;
			case ST_F_DCON:
				metric = mkf_u64(FN_COUNTER, l->counters->denied_conn);
				break;
			case ST_F_DSES:
				metric = mkf_u64(FN_COUNTER, l->counters->denied_sess);
				break;
			case ST_F_STATUS:
				metric = mkf_str(FO_STATUS, li_status_st[get_li_status(l)]);
				break;
			case ST_F_PID:
				metric = mkf_u32(FO_KEY, 1);
				break;
			case ST_F_IID:
				metric = mkf_u32(FO_KEY|FS_SERVICE, px->uuid);
				break;
			case ST_F_SID:
				metric = mkf_u32(FO_KEY|FS_SERVICE, l->luid);
				break;
			case ST_F_TYPE:
				metric = mkf_u32(FO_CONFIG|FS_SERVICE, STATS_TYPE_SO);
				break;
			case ST_F_WREW:
				metric = mkf_u64(FN_COUNTER, l->counters->failed_rewrites);
				break;
			case ST_F_EINT:
				metric = mkf_u64(FN_COUNTER, l->counters->internal_errors);
				break;
			case ST_F_ADDR:
				if (flags & STAT_SHLGNDS) {
					char str[INET6_ADDRSTRLEN];
					int port;

					port = get_host_port(&l->rx.addr);
					switch (addr_to_str(&l->rx.addr, str, sizeof(str))) {
					case AF_INET:
						metric = mkf_str(FO_CONFIG|FS_SERVICE, chunk_newstr(out));
						chunk_appendf(out, "%s:%d", str, port);
						break;
					case AF_INET6:
						metric = mkf_str(FO_CONFIG|FS_SERVICE, chunk_newstr(out));
						chunk_appendf(out, "[%s]:%d", str, port);
						break;
					case AF_UNIX:
						metric = mkf_str(FO_CONFIG|FS_SERVICE, "unix");
						break;
					case -1:
						metric = mkf_str(FO_CONFIG|FS_SERVICE, chunk_newstr(out));
						chunk_strcat(out, strerror(errno));
						break;
					default: /* address family not supported */
						break;
					}
				}
				break;
			case ST_F_PROTO:
				metric = mkf_str(FO_STATUS, l->rx.proto->name);
				break;
			default:
				/* not used for listen. If a specific metric
				 * is requested, return an error. Otherwise continue.
				 */
				if (selected_field != NULL)
					return 0;
				continue;
		}
		stats[current_field] = metric;
		if (selected_field != NULL)
			break;
	}
	return 1;
}

/* Dumps a line for listener <l> and proxy <px> to the local trash buffer and
 * uses the state from stream connector <sc>. The caller is responsible for
 * clearing the local trash buffer if needed. Returns non-zero if it emits
 * anything, zero otherwise.
 */
static int stats_dump_li_stats(struct stconn *sc, struct proxy *px, struct listener *l)
{
	struct appctx *appctx = __sc_appctx(sc);
	struct show_stat_ctx *ctx = appctx->svcctx;
	struct field *stats = stat_l[STATS_DOMAIN_PROXY];
	struct stats_module *mod;
	size_t stats_count = ST_F_TOTAL_FIELDS;

	memset(stats, 0, sizeof(struct field) * stat_count[STATS_DOMAIN_PROXY]);

	if (!stats_fill_li_stats(px, l, ctx->flags, stats,
				 ST_F_TOTAL_FIELDS, NULL))
		return 0;

	list_for_each_entry(mod, &stats_module_list[STATS_DOMAIN_PROXY], list) {
		void *counters;

		if (!(stats_px_get_cap(mod->domain_flags) & STATS_PX_CAP_LI)) {
			stats_count += mod->stats_count;
			continue;
		}

		counters = EXTRA_COUNTERS_GET(l->extra_counters, mod);
		mod->fill_stats(counters, stats + stats_count);
		stats_count += mod->stats_count;
	}

	return stats_dump_one_line(stats, stats_count, appctx);
}

enum srv_stats_state {
	SRV_STATS_STATE_DOWN = 0,
	SRV_STATS_STATE_DOWN_AGENT,
	SRV_STATS_STATE_GOING_UP,
	SRV_STATS_STATE_UP_GOING_DOWN,
	SRV_STATS_STATE_UP,
	SRV_STATS_STATE_NOLB_GOING_DOWN,
	SRV_STATS_STATE_NOLB,
	SRV_STATS_STATE_DRAIN_GOING_DOWN,
	SRV_STATS_STATE_DRAIN,
	SRV_STATS_STATE_DRAIN_AGENT,
	SRV_STATS_STATE_NO_CHECK,

	SRV_STATS_STATE_COUNT, /* Must be last */
};

static const char *srv_hlt_st[SRV_STATS_STATE_COUNT] = {
	[SRV_STATS_STATE_DOWN]			= "DOWN",
	[SRV_STATS_STATE_DOWN_AGENT]		= "DOWN (agent)",
	[SRV_STATS_STATE_GOING_UP]		= "DOWN %d/%d",
	[SRV_STATS_STATE_UP_GOING_DOWN]		= "UP %d/%d",
	[SRV_STATS_STATE_UP]			= "UP",
	[SRV_STATS_STATE_NOLB_GOING_DOWN]	= "NOLB %d/%d",
	[SRV_STATS_STATE_NOLB]			= "NOLB",
	[SRV_STATS_STATE_DRAIN_GOING_DOWN]	= "DRAIN %d/%d",
	[SRV_STATS_STATE_DRAIN]			= "DRAIN",
	[SRV_STATS_STATE_DRAIN_AGENT]		= "DRAIN (agent)",
	[SRV_STATS_STATE_NO_CHECK]		= "no check"
};

/* Compute server state helper
 */
static void stats_fill_sv_stats_computestate(struct server *sv, struct server *ref,
					     enum srv_stats_state *state)
{
	if (sv->cur_state == SRV_ST_RUNNING || sv->cur_state == SRV_ST_STARTING) {
		if ((ref->check.state & CHK_ST_ENABLED) &&
		    (ref->check.health < ref->check.rise + ref->check.fall - 1)) {
			*state = SRV_STATS_STATE_UP_GOING_DOWN;
		} else {
			*state = SRV_STATS_STATE_UP;
		}

		if (sv->cur_admin & SRV_ADMF_DRAIN) {
			if (ref->agent.state & CHK_ST_ENABLED)
				*state = SRV_STATS_STATE_DRAIN_AGENT;
			else if (*state == SRV_STATS_STATE_UP_GOING_DOWN)
				*state = SRV_STATS_STATE_DRAIN_GOING_DOWN;
			else
				*state = SRV_STATS_STATE_DRAIN;
		}

		if (*state == SRV_STATS_STATE_UP && !(ref->check.state & CHK_ST_ENABLED)) {
			*state = SRV_STATS_STATE_NO_CHECK;
		}
	}
	else if (sv->cur_state == SRV_ST_STOPPING) {
		if ((!(sv->check.state & CHK_ST_ENABLED) && !sv->track) ||
		    (ref->check.health == ref->check.rise + ref->check.fall - 1)) {
			*state = SRV_STATS_STATE_NOLB;
		} else {
			*state = SRV_STATS_STATE_NOLB_GOING_DOWN;
		}
	}
	else {	/* stopped */
		if ((ref->agent.state & CHK_ST_ENABLED) && !ref->agent.health) {
			*state = SRV_STATS_STATE_DOWN_AGENT;
		} else if ((ref->check.state & CHK_ST_ENABLED) && !ref->check.health) {
			*state = SRV_STATS_STATE_DOWN; /* DOWN */
		} else if ((ref->agent.state & CHK_ST_ENABLED) || (ref->check.state & CHK_ST_ENABLED)) {
			*state = SRV_STATS_STATE_GOING_UP;
		} else {
			*state = SRV_STATS_STATE_DOWN; /* DOWN, unchecked */
		}
	}
}

/* Fill <stats> with the backend statistics. <stats> is preallocated array of
 * length <len>. If <selected_field> is != NULL, only fill this one. The length
 * of the array must be at least ST_F_TOTAL_FIELDS. If this length is less than
 * this value, or if the selected field is not implemented for servers, the
 * function returns 0, otherwise, it returns 1. <flags> can take the value
 * STAT_SHLGNDS.
 */
int stats_fill_sv_stats(struct proxy *px, struct server *sv, int flags,
                        struct field *stats, int len,
			enum stat_field *selected_field)
{
	enum stat_field current_field = (selected_field != NULL ? *selected_field : 0);
	struct server *via = sv->track ? sv->track : sv;
	struct server *ref = via;
	enum srv_stats_state state = 0;
	char str[INET6_ADDRSTRLEN];
	struct buffer *out = get_trash_chunk();
	char *fld_status;
	long long srv_samples_counter;
	unsigned int srv_samples_window = TIME_STATS_SAMPLES;

	if (len < ST_F_TOTAL_FIELDS)
		return 0;

	chunk_reset(out);

	/* compute state for later use */
	if (selected_field == NULL || *selected_field == ST_F_STATUS ||
	    *selected_field == ST_F_CHECK_RISE || *selected_field == ST_F_CHECK_FALL ||
	    *selected_field == ST_F_CHECK_HEALTH || *selected_field == ST_F_HANAFAIL) {
		/* we have "via" which is the tracked server as described in the configuration,
		 * and "ref" which is the checked server and the end of the chain.
		 */
		while (ref->track)
			ref = ref->track;
		stats_fill_sv_stats_computestate(sv, ref, &state);
	}

	/* compue time values for later use */
	if (selected_field == NULL || *selected_field == ST_F_QTIME ||
	    *selected_field == ST_F_CTIME || *selected_field == ST_F_RTIME ||
	    *selected_field == ST_F_TTIME) {
		srv_samples_counter = (px->mode == PR_MODE_HTTP) ? sv->counters.p.http.cum_req : sv->counters.cum_lbconn;
		if (srv_samples_counter < TIME_STATS_SAMPLES && srv_samples_counter > 0)
			srv_samples_window = srv_samples_counter;
	}

	for (; current_field < ST_F_TOTAL_FIELDS; current_field++) {
		struct field metric = { 0 };

		switch (current_field) {
			case ST_F_PXNAME:
				metric = mkf_str(FO_KEY|FN_NAME|FS_SERVICE, px->id);
				break;
			case ST_F_SVNAME:
				metric = mkf_str(FO_KEY|FN_NAME|FS_SERVICE, sv->id);
				break;
			case ST_F_MODE:
				metric = mkf_str(FO_CONFIG|FS_SERVICE, proxy_mode_str(px->mode));
				break;
			case ST_F_QCUR:
				metric = mkf_u32(0, sv->queue.length);
				break;
			case ST_F_QMAX:
				metric = mkf_u32(FN_MAX, sv->counters.nbpend_max);
				break;
			case ST_F_SCUR:
				metric = mkf_u32(0, sv->cur_sess);
				break;
			case ST_F_SMAX:
				metric = mkf_u32(FN_MAX, sv->counters.cur_sess_max);
				break;
			case ST_F_SLIM:
				if (sv->maxconn)
					metric = mkf_u32(FO_CONFIG|FN_LIMIT, sv->maxconn);
				break;
			case ST_F_SRV_ICUR:
				metric = mkf_u32(0, sv->curr_idle_conns);
				break;
			case ST_F_SRV_ILIM:
				if (sv->max_idle_conns != -1)
					metric = mkf_u32(FO_CONFIG|FN_LIMIT, sv->max_idle_conns);
				break;
			case ST_F_STOT:
				metric = mkf_u64(FN_COUNTER, sv->counters.cum_sess);
				break;
			case ST_F_BIN:
				metric = mkf_u64(FN_COUNTER, sv->counters.bytes_in);
				break;
			case ST_F_BOUT:
				metric = mkf_u64(FN_COUNTER, sv->counters.bytes_out);
				break;
			case ST_F_DRESP:
				metric = mkf_u64(FN_COUNTER, sv->counters.denied_resp);
				break;
			case ST_F_ECON:
				metric = mkf_u64(FN_COUNTER, sv->counters.failed_conns);
				break;
			case ST_F_ERESP:
				metric = mkf_u64(FN_COUNTER, sv->counters.failed_resp);
				break;
			case ST_F_WRETR:
				metric = mkf_u64(FN_COUNTER, sv->counters.retries);
				break;
			case ST_F_WREDIS:
				metric = mkf_u64(FN_COUNTER, sv->counters.redispatches);
				break;
			case ST_F_WREW:
				metric = mkf_u64(FN_COUNTER, sv->counters.failed_rewrites);
				break;
			case ST_F_EINT:
				metric = mkf_u64(FN_COUNTER, sv->counters.internal_errors);
				break;
			case ST_F_CONNECT:
				metric = mkf_u64(FN_COUNTER, sv->counters.connect);
				break;
			case ST_F_REUSE:
				metric = mkf_u64(FN_COUNTER, sv->counters.reuse);
				break;
			case ST_F_IDLE_CONN_CUR:
				metric = mkf_u32(0, sv->curr_idle_nb);
				break;
			case ST_F_SAFE_CONN_CUR:
				metric = mkf_u32(0, sv->curr_safe_nb);
				break;
			case ST_F_USED_CONN_CUR:
				metric = mkf_u32(0, sv->curr_used_conns);
				break;
			case ST_F_NEED_CONN_EST:
				metric = mkf_u32(0, sv->est_need_conns);
				break;
			case ST_F_STATUS:
				fld_status = chunk_newstr(out);
				if (sv->cur_admin & SRV_ADMF_RMAINT)
					chunk_appendf(out, "MAINT (resolution)");
				else if (sv->cur_admin & SRV_ADMF_IMAINT)
					chunk_appendf(out, "MAINT (via %s/%s)", via->proxy->id, via->id);
				else if (sv->cur_admin & SRV_ADMF_MAINT)
					chunk_appendf(out, "MAINT");
				else
					chunk_appendf(out,
						      srv_hlt_st[state],
						      (ref->cur_state != SRV_ST_STOPPED) ? (ref->check.health - ref->check.rise + 1) : (ref->check.health),
						      (ref->cur_state != SRV_ST_STOPPED) ? (ref->check.fall) : (ref->check.rise));

				metric = mkf_str(FO_STATUS, fld_status);
				break;
			case ST_F_LASTCHG:
				metric = mkf_u32(FN_AGE, ns_to_sec(now_ns) - sv->last_change);
				break;
			case ST_F_WEIGHT:
				metric = mkf_u32(FN_AVG, (sv->cur_eweight * px->lbprm.wmult + px->lbprm.wdiv - 1) / px->lbprm.wdiv);
				break;
			case ST_F_UWEIGHT:
				metric = mkf_u32(FN_AVG, sv->uweight);
				break;
			case ST_F_ACT:
				metric = mkf_u32(FO_STATUS, (sv->flags & SRV_F_BACKUP) ? 0 : 1);
				break;
			case ST_F_BCK:
				metric = mkf_u32(FO_STATUS, (sv->flags & SRV_F_BACKUP) ? 1 : 0);
				break;
			case ST_F_CHKFAIL:
				if (sv->check.state & CHK_ST_ENABLED)
					metric = mkf_u64(FN_COUNTER, sv->counters.failed_checks);
				break;
			case ST_F_CHKDOWN:
				if (sv->check.state & CHK_ST_ENABLED)
					metric = mkf_u64(FN_COUNTER, sv->counters.down_trans);
				break;
			case ST_F_DOWNTIME:
				if (sv->check.state & CHK_ST_ENABLED)
					metric = mkf_u32(FN_COUNTER, srv_downtime(sv));
				break;
			case ST_F_QLIMIT:
				if (sv->maxqueue)
					metric = mkf_u32(FO_CONFIG|FS_SERVICE, sv->maxqueue);
				break;
			case ST_F_PID:
				metric = mkf_u32(FO_KEY, 1);
				break;
			case ST_F_IID:
				metric = mkf_u32(FO_KEY|FS_SERVICE, px->uuid);
				break;
			case ST_F_SID:
				metric = mkf_u32(FO_KEY|FS_SERVICE, sv->puid);
				break;
			case ST_F_SRID:
				metric = mkf_u32(FN_COUNTER, sv->rid);
				break;
			case ST_F_THROTTLE:
				if (sv->cur_state == SRV_ST_STARTING && !server_is_draining(sv))
					metric = mkf_u32(FN_AVG, server_throttle_rate(sv));
				break;
			case ST_F_LBTOT:
				metric = mkf_u64(FN_COUNTER, sv->counters.cum_lbconn);
				break;
			case ST_F_TRACKED:
				if (sv->track) {
					char *fld_track = chunk_newstr(out);
					chunk_appendf(out, "%s/%s", sv->track->proxy->id, sv->track->id);
					metric = mkf_str(FO_CONFIG|FN_NAME|FS_SERVICE, fld_track);
				}
				break;
			case ST_F_TYPE:
				metric = mkf_u32(FO_CONFIG|FS_SERVICE, STATS_TYPE_SV);
				break;
			case ST_F_RATE:
				metric = mkf_u32(FN_RATE, read_freq_ctr(&sv->sess_per_sec));
				break;
			case ST_F_RATE_MAX:
				metric = mkf_u32(FN_MAX, sv->counters.sps_max);
				break;
			case ST_F_CHECK_STATUS:
				if ((sv->check.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED) {
					const char *fld_chksts;

					fld_chksts = chunk_newstr(out);
					chunk_strcat(out, "* "); // for check in progress
					chunk_strcat(out, get_check_status_info(sv->check.status));
					if (!(sv->check.state & CHK_ST_INPROGRESS))
						fld_chksts += 2; // skip "* "
					metric = mkf_str(FN_OUTPUT, fld_chksts);
				}
				break;
			case ST_F_CHECK_CODE:
				if ((sv->check.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED &&
					sv->check.status >= HCHK_STATUS_L57DATA)
					metric = mkf_u32(FN_OUTPUT, sv->check.code);
				break;
			case ST_F_CHECK_DURATION:
				if ((sv->check.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED &&
					sv->check.status >= HCHK_STATUS_CHECKED)
					metric = mkf_u64(FN_DURATION, MAX(sv->check.duration, 0));
				break;
			case ST_F_CHECK_DESC:
				if ((sv->check.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED)
					metric = mkf_str(FN_OUTPUT, get_check_status_description(sv->check.status));
				break;
			case ST_F_LAST_CHK:
				if ((sv->check.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED)
					metric = mkf_str(FN_OUTPUT, sv->check.desc);
				break;
			case ST_F_CHECK_RISE:
				if ((sv->check.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED)
					metric = mkf_u32(FO_CONFIG|FS_SERVICE, ref->check.rise);
				break;
			case ST_F_CHECK_FALL:
				if ((sv->check.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED)
					metric = mkf_u32(FO_CONFIG|FS_SERVICE, ref->check.fall);
				break;
			case ST_F_CHECK_HEALTH:
				if ((sv->check.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED)
					metric = mkf_u32(FO_CONFIG|FS_SERVICE, ref->check.health);
				break;
			case ST_F_AGENT_STATUS:
				if  ((sv->agent.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED) {
					const char *fld_chksts;

					fld_chksts = chunk_newstr(out);
					chunk_strcat(out, "* "); // for check in progress
					chunk_strcat(out, get_check_status_info(sv->agent.status));
					if (!(sv->agent.state & CHK_ST_INPROGRESS))
						fld_chksts += 2; // skip "* "
					metric = mkf_str(FN_OUTPUT, fld_chksts);
				}
				break;
			case ST_F_AGENT_CODE:
				if  ((sv->agent.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED &&
				     (sv->agent.status >= HCHK_STATUS_L57DATA))
					metric = mkf_u32(FN_OUTPUT, sv->agent.code);
				break;
			case ST_F_AGENT_DURATION:
				if ((sv->agent.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED)
					metric = mkf_u64(FN_DURATION, sv->agent.duration);
				break;
			case ST_F_AGENT_DESC:
				if ((sv->agent.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED)
					metric = mkf_str(FN_OUTPUT, get_check_status_description(sv->agent.status));
				break;
			case ST_F_LAST_AGT:
				if ((sv->agent.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED)
					metric = mkf_str(FN_OUTPUT, sv->agent.desc);
				break;
			case ST_F_AGENT_RISE:
				if ((sv->agent.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED)
					metric = mkf_u32(FO_CONFIG|FS_SERVICE, sv->agent.rise);
				break;
			case ST_F_AGENT_FALL:
				if ((sv->agent.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED)
					metric = mkf_u32(FO_CONFIG|FS_SERVICE, sv->agent.fall);
				break;
			case ST_F_AGENT_HEALTH:
				if ((sv->agent.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) == CHK_ST_ENABLED)
					metric = mkf_u32(FO_CONFIG|FS_SERVICE, sv->agent.health);
				break;
			case ST_F_REQ_TOT:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, sv->counters.p.http.cum_req);
				break;
			case ST_F_HRSP_1XX:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[1]);
				break;
			case ST_F_HRSP_2XX:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[2]);
				break;
			case ST_F_HRSP_3XX:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[3]);
				break;
			case ST_F_HRSP_4XX:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[4]);
				break;
			case ST_F_HRSP_5XX:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[5]);
				break;
			case ST_F_HRSP_OTHER:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[0]);
				break;
			case ST_F_HANAFAIL:
				if (ref->observe)
					metric = mkf_u64(FN_COUNTER, sv->counters.failed_hana);
				break;
			case ST_F_CLI_ABRT:
				metric = mkf_u64(FN_COUNTER, sv->counters.cli_aborts);
				break;
			case ST_F_SRV_ABRT:
				metric = mkf_u64(FN_COUNTER, sv->counters.srv_aborts);
				break;
			case ST_F_LASTSESS:
				metric = mkf_s32(FN_AGE, srv_lastsession(sv));
				break;
			case ST_F_QTIME:
				metric = mkf_u32(FN_AVG, swrate_avg(sv->counters.q_time, srv_samples_window));
				break;
			case ST_F_CTIME:
				metric = mkf_u32(FN_AVG, swrate_avg(sv->counters.c_time, srv_samples_window));
				break;
			case ST_F_RTIME:
				metric = mkf_u32(FN_AVG, swrate_avg(sv->counters.d_time, srv_samples_window));
				break;
			case ST_F_TTIME:
				metric = mkf_u32(FN_AVG, swrate_avg(sv->counters.t_time, srv_samples_window));
				break;
			case ST_F_QT_MAX:
				metric = mkf_u32(FN_MAX, sv->counters.qtime_max);
				break;
			case ST_F_CT_MAX:
				metric = mkf_u32(FN_MAX, sv->counters.ctime_max);
				break;
			case ST_F_RT_MAX:
				metric = mkf_u32(FN_MAX, sv->counters.dtime_max);
				break;
			case ST_F_TT_MAX:
				metric = mkf_u32(FN_MAX, sv->counters.ttime_max);
				break;
			case ST_F_ADDR:
				if (flags & STAT_SHLGNDS) {
					switch (addr_to_str(&sv->addr, str, sizeof(str))) {
						case AF_INET:
							metric = mkf_str(FO_CONFIG|FS_SERVICE, chunk_newstr(out));
							chunk_appendf(out, "%s:%d", str, sv->svc_port);
							break;
						case AF_INET6:
							metric = mkf_str(FO_CONFIG|FS_SERVICE, chunk_newstr(out));
							chunk_appendf(out, "[%s]:%d", str, sv->svc_port);
							break;
						case AF_UNIX:
							metric = mkf_str(FO_CONFIG|FS_SERVICE, "unix");
							break;
						case -1:
							metric = mkf_str(FO_CONFIG|FS_SERVICE, chunk_newstr(out));
							chunk_strcat(out, strerror(errno));
							break;
						default: /* address family not supported */
							break;
					}
				}
				break;
			case ST_F_COOKIE:
				if (flags & STAT_SHLGNDS && sv->cookie)
					metric = mkf_str(FO_CONFIG|FN_NAME|FS_SERVICE, sv->cookie);
				break;
			default:
				/* not used for servers. If a specific metric
				 * is requested, return an error. Otherwise continue.
				 */
				if (selected_field != NULL)
					return 0;
				continue;
		}
		stats[current_field] = metric;
		if (selected_field != NULL)
			break;
	}
	return 1;
}

/* Dumps a line for server <sv> and proxy <px> to the local trash vbuffer and
 * uses the state from stream connector <sc>, and server state <state>. The
 * caller is responsible for clearing the local trash buffer if needed. Returns
 * non-zero if it emits anything, zero otherwise.
 */
static int stats_dump_sv_stats(struct stconn *sc, struct proxy *px, struct server *sv)
{
	struct appctx *appctx = __sc_appctx(sc);
	struct show_stat_ctx *ctx = appctx->svcctx;
	struct stats_module *mod;
	struct field *stats = stat_l[STATS_DOMAIN_PROXY];
	size_t stats_count = ST_F_TOTAL_FIELDS;

	memset(stats, 0, sizeof(struct field) * stat_count[STATS_DOMAIN_PROXY]);

	if (!stats_fill_sv_stats(px, sv, ctx->flags, stats,
				 ST_F_TOTAL_FIELDS, NULL))
		return 0;

	list_for_each_entry(mod, &stats_module_list[STATS_DOMAIN_PROXY], list) {
		void *counters;

		if (stats_get_domain(mod->domain_flags) != STATS_DOMAIN_PROXY)
			continue;

		if (!(stats_px_get_cap(mod->domain_flags) & STATS_PX_CAP_SRV)) {
			stats_count += mod->stats_count;
			continue;
		}

		counters = EXTRA_COUNTERS_GET(sv->extra_counters, mod);
		mod->fill_stats(counters, stats + stats_count);
		stats_count += mod->stats_count;
	}

	return stats_dump_one_line(stats, stats_count, appctx);
}

/* Helper to compute srv values for a given backend
 */
static void stats_fill_be_stats_computesrv(struct proxy *px, int *nbup, int *nbsrv, int *totuw)
{
	int nbup_tmp, nbsrv_tmp, totuw_tmp;
	const struct server *srv;

	nbup_tmp = nbsrv_tmp = totuw_tmp = 0;
	for (srv = px->srv; srv; srv = srv->next) {
		if (srv->cur_state != SRV_ST_STOPPED) {
			nbup_tmp++;
			if (srv_currently_usable(srv) &&
			    (!px->srv_act ^ !(srv->flags & SRV_F_BACKUP)))
				totuw_tmp += srv->uweight;
		}
		nbsrv_tmp++;
	}

	HA_RWLOCK_RDLOCK(LBPRM_LOCK, &px->lbprm.lock);
	if (!px->srv_act && px->lbprm.fbck)
		totuw_tmp = px->lbprm.fbck->uweight;
	HA_RWLOCK_RDUNLOCK(LBPRM_LOCK, &px->lbprm.lock);

	/* use tmp variable then assign result to make gcc happy */
	*nbup = nbup_tmp;
	*nbsrv = nbsrv_tmp;
	*totuw = totuw_tmp;
}

/* Fill <stats> with the backend statistics. <stats> is preallocated array of
 * length <len>. If <selected_field> is != NULL, only fill this one. The length
 * of the array must be at least ST_F_TOTAL_FIELDS. If this length is less than
 * this value, or if the selected field is not implemented for backends, the
 * function returns 0, otherwise, it returns 1. <flags> can take the value
 * STAT_SHLGNDS.
 */
int stats_fill_be_stats(struct proxy *px, int flags, struct field *stats, int len,
			enum stat_field *selected_field)
{
	enum stat_field current_field = (selected_field != NULL ? *selected_field : 0);
	long long be_samples_counter;
	unsigned int be_samples_window = TIME_STATS_SAMPLES;
	struct buffer *out = get_trash_chunk();
	int nbup, nbsrv, totuw;
	char *fld;

	if (len < ST_F_TOTAL_FIELDS)
		return 0;

	nbup = nbsrv = totuw = 0;
	/* some srv values compute for later if we either select all fields or
	 * need them for one of the mentioned ones */
	if (selected_field == NULL || *selected_field == ST_F_STATUS ||
	    *selected_field == ST_F_UWEIGHT)
		stats_fill_be_stats_computesrv(px, &nbup, &nbsrv, &totuw);

	/* same here but specific to time fields */
	if (selected_field == NULL || *selected_field == ST_F_QTIME ||
	    *selected_field == ST_F_CTIME || *selected_field == ST_F_RTIME ||
	    *selected_field == ST_F_TTIME) {
		be_samples_counter = (px->mode == PR_MODE_HTTP) ? px->be_counters.p.http.cum_req : px->be_counters.cum_lbconn;
		if (be_samples_counter < TIME_STATS_SAMPLES && be_samples_counter > 0)
			be_samples_window = be_samples_counter;
	}

	for (; current_field < ST_F_TOTAL_FIELDS; current_field++) {
		struct field metric = { 0 };

		switch (current_field) {
			case ST_F_PXNAME:
				metric = mkf_str(FO_KEY|FN_NAME|FS_SERVICE, px->id);
				break;
			case ST_F_SVNAME:
				metric = mkf_str(FO_KEY|FN_NAME|FS_SERVICE, "BACKEND");
				break;
			case ST_F_MODE:
				metric = mkf_str(FO_CONFIG|FS_SERVICE, proxy_mode_str(px->mode));
				break;
			case ST_F_QCUR:
				metric = mkf_u32(0, px->queue.length);
				break;
			case ST_F_QMAX:
				metric = mkf_u32(FN_MAX, px->be_counters.nbpend_max);
				break;
			case ST_F_SCUR:
				metric = mkf_u32(0, px->beconn);
				break;
			case ST_F_SMAX:
				metric = mkf_u32(FN_MAX, px->be_counters.conn_max);
				break;
			case ST_F_SLIM:
				metric = mkf_u32(FO_CONFIG|FN_LIMIT, px->fullconn);
				break;
			case ST_F_STOT:
				metric = mkf_u64(FN_COUNTER, px->be_counters.cum_conn);
				break;
			case ST_F_BIN:
				metric = mkf_u64(FN_COUNTER, px->be_counters.bytes_in);
				break;
			case ST_F_BOUT:
				metric = mkf_u64(FN_COUNTER, px->be_counters.bytes_out);
				break;
			case ST_F_DREQ:
				metric = mkf_u64(FN_COUNTER, px->be_counters.denied_req);
				break;
			case ST_F_DRESP:
				metric = mkf_u64(FN_COUNTER, px->be_counters.denied_resp);
				break;
			case ST_F_ECON:
				metric = mkf_u64(FN_COUNTER, px->be_counters.failed_conns);
				break;
			case ST_F_ERESP:
				metric = mkf_u64(FN_COUNTER, px->be_counters.failed_resp);
				break;
			case ST_F_WRETR:
				metric = mkf_u64(FN_COUNTER, px->be_counters.retries);
				break;
			case ST_F_WREDIS:
				metric = mkf_u64(FN_COUNTER, px->be_counters.redispatches);
				break;
			case ST_F_WREW:
				metric = mkf_u64(FN_COUNTER, px->be_counters.failed_rewrites);
				break;
			case ST_F_EINT:
				metric = mkf_u64(FN_COUNTER, px->be_counters.internal_errors);
				break;
			case ST_F_CONNECT:
				metric = mkf_u64(FN_COUNTER, px->be_counters.connect);
				break;
			case ST_F_REUSE:
				metric = mkf_u64(FN_COUNTER, px->be_counters.reuse);
				break;
			case ST_F_STATUS:
				fld = chunk_newstr(out);
				chunk_appendf(out, "%s", (px->lbprm.tot_weight > 0 || !px->srv) ? "UP" : "DOWN");
				if (flags & (STAT_HIDE_MAINT|STAT_HIDE_DOWN))
					chunk_appendf(out, " (%d/%d)", nbup, nbsrv);
				metric = mkf_str(FO_STATUS, fld);
				break;
			case ST_F_AGG_SRV_CHECK_STATUS:   // DEPRECATED
			case ST_F_AGG_SRV_STATUS:
				metric = mkf_u32(FN_GAUGE, 0);
				break;
			case ST_F_AGG_CHECK_STATUS:
				metric = mkf_u32(FN_GAUGE, 0);
				break;
			case ST_F_WEIGHT:
				metric = mkf_u32(FN_AVG, (px->lbprm.tot_weight * px->lbprm.wmult + px->lbprm.wdiv - 1) / px->lbprm.wdiv);
				break;
			case ST_F_UWEIGHT:
				metric = mkf_u32(FN_AVG, totuw);
				break;
			case ST_F_ACT:
				metric = mkf_u32(0, px->srv_act);
				break;
			case ST_F_BCK:
				metric = mkf_u32(0, px->srv_bck);
				break;
			case ST_F_CHKDOWN:
				metric = mkf_u64(FN_COUNTER, px->down_trans);
				break;
			case ST_F_LASTCHG:
				metric = mkf_u32(FN_AGE, ns_to_sec(now_ns) - px->last_change);
				break;
			case ST_F_DOWNTIME:
				if (px->srv)
					metric = mkf_u32(FN_COUNTER, be_downtime(px));
				break;
			case ST_F_PID:
				metric = mkf_u32(FO_KEY, 1);
				break;
			case ST_F_IID:
				metric = mkf_u32(FO_KEY|FS_SERVICE, px->uuid);
				break;
			case ST_F_SID:
				metric = mkf_u32(FO_KEY|FS_SERVICE, 0);
				break;
			case ST_F_LBTOT:
				metric = mkf_u64(FN_COUNTER, px->be_counters.cum_lbconn);
				break;
			case ST_F_TYPE:
				metric = mkf_u32(FO_CONFIG|FS_SERVICE, STATS_TYPE_BE);
				break;
			case ST_F_RATE:
				metric = mkf_u32(0, read_freq_ctr(&px->be_sess_per_sec));
				break;
			case ST_F_RATE_MAX:
				metric = mkf_u32(0, px->be_counters.sps_max);
				break;
			case ST_F_COOKIE:
				if (flags & STAT_SHLGNDS && px->cookie_name)
					metric = mkf_str(FO_CONFIG|FN_NAME|FS_SERVICE, px->cookie_name);
				break;
			case ST_F_ALGO:
				if (flags & STAT_SHLGNDS)
					metric = mkf_str(FO_CONFIG|FS_SERVICE, backend_lb_algo_str(px->lbprm.algo & BE_LB_ALGO));
				break;
			case ST_F_REQ_TOT:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->be_counters.p.http.cum_req);
				break;
			case ST_F_HRSP_1XX:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[1]);
				break;
			case ST_F_HRSP_2XX:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[2]);
				break;
			case ST_F_HRSP_3XX:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[3]);
				break;
			case ST_F_HRSP_4XX:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[4]);
				break;
			case ST_F_HRSP_5XX:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[5]);
				break;
			case ST_F_HRSP_OTHER:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[0]);
				break;
			case ST_F_CACHE_LOOKUPS:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->be_counters.p.http.cache_lookups);
				break;
			case ST_F_CACHE_HITS:
				if (px->mode == PR_MODE_HTTP)
					metric = mkf_u64(FN_COUNTER, px->be_counters.p.http.cache_hits);
				break;
			case ST_F_CLI_ABRT:
				metric = mkf_u64(FN_COUNTER, px->be_counters.cli_aborts);
				break;
			case ST_F_SRV_ABRT:
				metric = mkf_u64(FN_COUNTER, px->be_counters.srv_aborts);
				break;
			case ST_F_COMP_IN:
				metric = mkf_u64(FN_COUNTER, px->be_counters.comp_in[COMP_DIR_RES]);
				break;
			case ST_F_COMP_OUT:
				metric = mkf_u64(FN_COUNTER, px->be_counters.comp_out[COMP_DIR_RES]);
				break;
			case ST_F_COMP_BYP:
				metric = mkf_u64(FN_COUNTER, px->be_counters.comp_byp[COMP_DIR_RES]);
				break;
			case ST_F_COMP_RSP:
				metric = mkf_u64(FN_COUNTER, px->be_counters.p.http.comp_rsp);
				break;
			case ST_F_LASTSESS:
				metric = mkf_s32(FN_AGE, be_lastsession(px));
				break;
			case ST_F_QTIME:
				metric = mkf_u32(FN_AVG, swrate_avg(px->be_counters.q_time, be_samples_window));
				break;
			case ST_F_CTIME:
				metric = mkf_u32(FN_AVG, swrate_avg(px->be_counters.c_time, be_samples_window));
				break;
			case ST_F_RTIME:
				metric = mkf_u32(FN_AVG, swrate_avg(px->be_counters.d_time, be_samples_window));
				break;
			case ST_F_TTIME:
				metric = mkf_u32(FN_AVG, swrate_avg(px->be_counters.t_time, be_samples_window));
				break;
			case ST_F_QT_MAX:
				metric = mkf_u32(FN_MAX, px->be_counters.qtime_max);
				break;
			case ST_F_CT_MAX:
				metric = mkf_u32(FN_MAX, px->be_counters.ctime_max);
				break;
			case ST_F_RT_MAX:
				metric = mkf_u32(FN_MAX, px->be_counters.dtime_max);
				break;
			case ST_F_TT_MAX:
				metric = mkf_u32(FN_MAX, px->be_counters.ttime_max);
				break;
			default:
				/* not used for backends. If a specific metric
				 * is requested, return an error. Otherwise continue.
				 */
				if (selected_field != NULL)
					return 0;
				continue;
		}
		stats[current_field] = metric;
		if (selected_field != NULL)
			break;
	}
	return 1;
}

/* Dumps a line for backend <px> to the local trash buffer for and uses the
 * state from stream interface <si>. The caller is responsible for clearing the
 * local trash buffer if needed.  Returns non-zero if it emits anything, zero
 * otherwise.
 */
static int stats_dump_be_stats(struct stconn *sc, struct proxy *px)
{
	struct appctx *appctx = __sc_appctx(sc);
	struct show_stat_ctx *ctx = appctx->svcctx;
	struct field *stats = stat_l[STATS_DOMAIN_PROXY];
	struct stats_module *mod;
	size_t stats_count = ST_F_TOTAL_FIELDS;

	if (!(px->cap & PR_CAP_BE))
		return 0;

	if ((ctx->flags & STAT_BOUND) && !(ctx->type & (1 << STATS_TYPE_BE)))
		return 0;

	memset(stats, 0, sizeof(struct field) * stat_count[STATS_DOMAIN_PROXY]);

	if (!stats_fill_be_stats(px, ctx->flags, stats, ST_F_TOTAL_FIELDS, NULL))
		return 0;

	list_for_each_entry(mod, &stats_module_list[STATS_DOMAIN_PROXY], list) {
		struct extra_counters *counters;

		if (stats_get_domain(mod->domain_flags) != STATS_DOMAIN_PROXY)
			continue;

		if (!(stats_px_get_cap(mod->domain_flags) & STATS_PX_CAP_BE)) {
			stats_count += mod->stats_count;
			continue;
		}

		counters = EXTRA_COUNTERS_GET(px->extra_counters_be, mod);
		mod->fill_stats(counters, stats + stats_count);
		stats_count += mod->stats_count;
	}

	return stats_dump_one_line(stats, stats_count, appctx);
}

/* Dumps the HTML table header for proxy <px> to the local trash buffer for and
 * uses the state from stream connector <sc> and per-uri parameters <uri>. The
 * caller is responsible for clearing the local trash buffer if needed.
 */
static void stats_dump_html_px_hdr(struct stconn *sc, struct proxy *px)
{
	struct appctx *appctx = __sc_appctx(sc);
	struct show_stat_ctx *ctx = appctx->svcctx;
	char scope_txt[STAT_SCOPE_TXT_MAXLEN + sizeof STAT_SCOPE_PATTERN];
	struct stats_module *mod;
	int stats_module_len = 0;

	if (px->cap & PR_CAP_BE && px->srv && (ctx->flags & STAT_ADMIN)) {
		/* A form to enable/disable this proxy servers */

		/* scope_txt = search pattern + search query, ctx->scope_len is always <= STAT_SCOPE_TXT_MAXLEN */
		scope_txt[0] = 0;
		if (ctx->scope_len) {
			const char *scope_ptr = stats_scope_ptr(appctx, sc);

			strlcpy2(scope_txt, STAT_SCOPE_PATTERN, sizeof(scope_txt));
			memcpy(scope_txt + strlen(STAT_SCOPE_PATTERN), scope_ptr, ctx->scope_len);
			scope_txt[strlen(STAT_SCOPE_PATTERN) + ctx->scope_len] = 0;
		}

		chunk_appendf(&trash_chunk,
			      "<form method=\"post\">");
	}

	/* print a new table */
	chunk_appendf(&trash_chunk,
		      "<table class=\"tbl\" width=\"100%%\">\n"
		      "<tr class=\"titre\">"
		      "<th class=\"pxname\" width=\"10%%\">");

	chunk_appendf(&trash_chunk,
	              "<a name=\"%s\"></a>%s"
	              "<a class=px href=\"#%s\">%s</a>",
	              px->id,
	              (ctx->flags & STAT_SHLGNDS) ? "<u>":"",
	              px->id, px->id);

	if (ctx->flags & STAT_SHLGNDS) {
		/* cap, mode, id */
		chunk_appendf(&trash_chunk, "<div class=tips>cap: %s, mode: %s, id: %d",
		              proxy_cap_str(px->cap), proxy_mode_str(px->mode),
		              px->uuid);
		chunk_appendf(&trash_chunk, "</div>");
	}

	chunk_appendf(&trash_chunk,
	              "%s</th>"
	              "<th class=\"%s\" width=\"90%%\">%s</th>"
	              "</tr>\n"
	              "</table>\n"
	              "<table class=\"tbl\" width=\"100%%\">\n"
	              "<tr class=\"titre\">",
	              (ctx->flags & STAT_SHLGNDS) ? "</u>":"",
	              px->desc ? "desc" : "empty", px->desc ? px->desc : "");

	if (ctx->flags & STAT_ADMIN) {
		/* Column heading for Enable or Disable server */
		if ((px->cap & PR_CAP_BE) && px->srv)
			chunk_appendf(&trash_chunk,
				      "<th rowspan=2 width=1><input type=\"checkbox\" "
				      "onclick=\"for(c in document.getElementsByClassName('%s-checkbox')) "
				      "document.getElementsByClassName('%s-checkbox').item(c).checked = this.checked\"></th>",
				      px->id,
				      px->id);
		else
			chunk_appendf(&trash_chunk, "<th rowspan=2></th>");
	}

	chunk_appendf(&trash_chunk,
	              "<th rowspan=2></th>"
	              "<th colspan=3>Queue</th>"
	              "<th colspan=3>Session rate</th><th colspan=6>Sessions</th>"
	              "<th colspan=2>Bytes</th><th colspan=2>Denied</th>"
	              "<th colspan=3>Errors</th><th colspan=2>Warnings</th>"
	              "<th colspan=9>Server</th>");

	if (ctx->flags & STAT_SHMODULES) {
		// calculate the count of module for colspan attribute
		list_for_each_entry(mod, &stats_module_list[STATS_DOMAIN_PROXY], list) {
			++stats_module_len;
		}
		chunk_appendf(&trash_chunk, "<th colspan=%d>Extra modules</th>",
		              stats_module_len);
	}

	chunk_appendf(&trash_chunk,
	              "</tr>\n"
	              "<tr class=\"titre\">"
	              "<th>Cur</th><th>Max</th><th>Limit</th>"
	              "<th>Cur</th><th>Max</th><th>Limit</th><th>Cur</th><th>Max</th>"
	              "<th>Limit</th><th>Total</th><th>LbTot</th><th>Last</th><th>In</th><th>Out</th>"
	              "<th>Req</th><th>Resp</th><th>Req</th><th>Conn</th>"
	              "<th>Resp</th><th>Retr</th><th>Redis</th>"
	              "<th>Status</th><th>LastChk</th><th>Wght</th><th>Act</th>"
	              "<th>Bck</th><th>Chk</th><th>Dwn</th><th>Dwntme</th>"
	              "<th>Thrtle</th>\n");

	if (ctx->flags & STAT_SHMODULES) {
		list_for_each_entry(mod, &stats_module_list[STATS_DOMAIN_PROXY], list) {
			chunk_appendf(&trash_chunk, "<th>%s</th>", mod->name);
		}
	}

	chunk_appendf(&trash_chunk, "</tr>");
}

/* Dumps the HTML table trailer for proxy <px> to the local trash buffer for and
 * uses the state from stream connector <sc>. The caller is responsible for
 * clearing the local trash buffer if needed.
 */
static void stats_dump_html_px_end(struct stconn *sc, struct proxy *px)
{
	struct appctx *appctx = __sc_appctx(sc);
	struct show_stat_ctx *ctx = appctx->svcctx;

	chunk_appendf(&trash_chunk, "</table>");

	if ((px->cap & PR_CAP_BE) && px->srv && (ctx->flags & STAT_ADMIN)) {
		/* close the form used to enable/disable this proxy servers */
		chunk_appendf(&trash_chunk,
			      "Choose the action to perform on the checked servers : "
			      "<select name=action>"
			      "<option value=\"\"></option>"
			      "<option value=\"ready\">Set state to READY</option>"
			      "<option value=\"drain\">Set state to DRAIN</option>"
			      "<option value=\"maint\">Set state to MAINT</option>"
			      "<option value=\"dhlth\">Health: disable checks</option>"
			      "<option value=\"ehlth\">Health: enable checks</option>"
			      "<option value=\"hrunn\">Health: force UP</option>"
			      "<option value=\"hnolb\">Health: force NOLB</option>"
			      "<option value=\"hdown\">Health: force DOWN</option>"
			      "<option value=\"dagent\">Agent: disable checks</option>"
			      "<option value=\"eagent\">Agent: enable checks</option>"
			      "<option value=\"arunn\">Agent: force UP</option>"
			      "<option value=\"adown\">Agent: force DOWN</option>"
			      "<option value=\"shutdown\">Kill Sessions</option>"
			      "</select>"
			      "<input type=\"hidden\" name=\"b\" value=\"#%d\">"
			      "&nbsp;<input type=\"submit\" value=\"Apply\">"
			      "</form>",
			      px->uuid);
	}

	chunk_appendf(&trash_chunk, "<p>\n");
}

/*
 * Dumps statistics for a proxy. The output is sent to the stream connector's
 * input buffer. Returns 0 if it had to stop dumping data because of lack of
 * buffer space, or non-zero if everything completed. This function is used
 * both by the CLI and the HTTP entry points, and is able to dump the output
 * in HTML or CSV formats. If the later, <uri> must be NULL.
 */
int stats_dump_proxy_to_buffer(struct stconn *sc, struct htx *htx,
			       struct proxy *px, struct uri_auth *uri)
{
	struct appctx *appctx = __sc_appctx(sc);
	struct show_stat_ctx *ctx = appctx->svcctx;
	struct stream *s = __sc_strm(sc);
	struct channel *rep = sc_ic(sc);
	struct server *sv, *svs;	/* server and server-state, server-state=server or server->track */
	struct listener *l;
	int current_field;
	int px_st = ctx->px_st;

	chunk_reset(&trash_chunk);
more:
	current_field = ctx->field;

	switch (ctx->px_st) {
	case STAT_PX_ST_INIT:
		/* we are on a new proxy */
		if (uri && uri->scope) {
			/* we have a limited scope, we have to check the proxy name */
			struct stat_scope *scope;
			int len;

			len = strlen(px->id);
			scope = uri->scope;

			while (scope) {
				/* match exact proxy name */
				if (scope->px_len == len && !memcmp(px->id, scope->px_id, len))
					break;

				/* match '.' which means 'self' proxy */
				if (strcmp(scope->px_id, ".") == 0 && px == s->be)
					break;
				scope = scope->next;
			}

			/* proxy name not found : don't dump anything */
			if (scope == NULL)
				return 1;
		}

		/* if the user has requested a limited output and the proxy
		 * name does not match, skip it.
		 */
		if (ctx->scope_len) {
			const char *scope_ptr = stats_scope_ptr(appctx, sc);

			if (strnistr(px->id, strlen(px->id), scope_ptr, ctx->scope_len) == NULL)
				return 1;
		}

		if ((ctx->flags & STAT_BOUND) &&
		    (ctx->iid != -1) &&
		    (px->uuid != ctx->iid))
			return 1;

		ctx->px_st = STAT_PX_ST_TH;
		__fallthrough;

	case STAT_PX_ST_TH:
		if (ctx->flags & STAT_FMT_HTML) {
			stats_dump_html_px_hdr(sc, px);
			if (!stats_putchk(appctx, htx))
				goto full;
		}

		ctx->px_st = STAT_PX_ST_FE;
		__fallthrough;

	case STAT_PX_ST_FE:
		/* print the frontend */
		if (stats_dump_fe_stats(sc, px)) {
			if (!stats_putchk(appctx, htx))
				goto full;
			ctx->flags |= STAT_STARTED;
			if (ctx->field)
				goto more;
		}

		current_field = 0;
		ctx->obj2 = px->conf.listeners.n;
		ctx->px_st = STAT_PX_ST_LI;
		__fallthrough;

	case STAT_PX_ST_LI:
		/* obj2 points to listeners list as initialized above */
		for (; ctx->obj2 != &px->conf.listeners; ctx->obj2 = l->by_fe.n) {
			if (htx) {
				if (htx_almost_full(htx)) {
					sc_need_room(sc, htx->size / 2);
					goto full;
				}
			}
			else {
				if (buffer_almost_full(&rep->buf)) {
					sc_need_room(sc, b_size(&rep->buf) / 2);
					goto full;
				}
			}

			l = LIST_ELEM(ctx->obj2, struct listener *, by_fe);
			if (!l->counters)
				continue;

			if (ctx->flags & STAT_BOUND) {
				if (!(ctx->type & (1 << STATS_TYPE_SO)))
					break;

				if (ctx->sid != -1 && l->luid != ctx->sid)
					continue;
			}

			/* print the frontend */
			if (stats_dump_li_stats(sc, px, l)) {
				if (!stats_putchk(appctx, htx))
					goto full;
				ctx->flags |= STAT_STARTED;
				if (ctx->field)
					goto more;
			}
			current_field = 0;
		}

		ctx->obj2 = px->srv; /* may be NULL */
		ctx->px_st = STAT_PX_ST_SV;
		__fallthrough;

	case STAT_PX_ST_SV:
		/* check for dump resumption */
		if (px_st == STAT_PX_ST_SV) {
			struct server *cur = ctx->obj2;

			/* re-entrant dump */
			BUG_ON(!cur);
			if (cur->flags & SRV_F_DELETED) {
				/* the server could have been marked as deleted
				 * between two dumping attempts, skip it.
				 */
				cur = cur->next;
			}
			srv_drop(ctx->obj2); /* drop old srv taken on last dumping attempt */
			ctx->obj2 = cur; /* could be NULL */
			/* back to normal */
		}

		/* obj2 points to servers list as initialized above.
		 *
		 * A server may be removed during the stats dumping.
		 * Temporarily increment its refcount to prevent its
		 * anticipated cleaning. Call srv_drop() to release it.
		 */
		for (; ctx->obj2 != NULL;
		       ctx->obj2 = srv_drop(sv)) {

			sv = ctx->obj2;
			srv_take(sv);

			if (htx) {
				if (htx_almost_full(htx)) {
					sc_need_room(sc, htx->size / 2);
					goto full;
				}
			}
			else {
				if (buffer_almost_full(&rep->buf)) {
					sc_need_room(sc, b_size(&rep->buf) / 2);
					goto full;
				}
			}

			if (ctx->flags & STAT_BOUND) {
				if (!(ctx->type & (1 << STATS_TYPE_SV))) {
					srv_drop(sv);
					break;
				}

				if (ctx->sid != -1 && sv->puid != ctx->sid)
					continue;
			}

			/* do not report disabled servers */
			if (ctx->flags & STAT_HIDE_MAINT &&
			    sv->cur_admin & SRV_ADMF_MAINT) {
				continue;
			}

			svs = sv;
			while (svs->track)
				svs = svs->track;

			/* do not report servers which are DOWN and not changing state */
			if ((ctx->flags & STAT_HIDE_DOWN) &&
			    ((sv->cur_admin & SRV_ADMF_MAINT) || /* server is in maintenance */
			     (sv->cur_state == SRV_ST_STOPPED && /* server is down */
			      (!((svs->agent.state | svs->check.state) & CHK_ST_ENABLED) ||
			       ((svs->agent.state & CHK_ST_ENABLED) && !svs->agent.health) ||
			       ((svs->check.state & CHK_ST_ENABLED) && !svs->check.health))))) {
				continue;
			}

			if (stats_dump_sv_stats(sc, px, sv)) {
				if (!stats_putchk(appctx, htx))
					goto full;
				ctx->flags |= STAT_STARTED;
				if (ctx->field)
					goto more;
			}
			current_field = 0;
		} /* for sv */

		ctx->px_st = STAT_PX_ST_BE;
		__fallthrough;

	case STAT_PX_ST_BE:
		/* print the backend */
		if (stats_dump_be_stats(sc, px)) {
			if (!stats_putchk(appctx, htx))
				goto full;
			ctx->flags |= STAT_STARTED;
			if (ctx->field)
				goto more;
		}

		current_field = 0;
		ctx->px_st = STAT_PX_ST_END;
		__fallthrough;

	case STAT_PX_ST_END:
		if (ctx->flags & STAT_FMT_HTML) {
			stats_dump_html_px_end(sc, px);
			if (!stats_putchk(appctx, htx))
				goto full;
		}

		ctx->px_st = STAT_PX_ST_FIN;
		__fallthrough;

	case STAT_PX_ST_FIN:
		return 1;

	default:
		/* unknown state, we should put an abort() here ! */
		return 1;
	}

  full:
	/* restore previous field */
	ctx->field = current_field;
	return 0;
}

/* Dumps the HTTP stats head block to the local trash buffer for and uses the
 * per-uri parameters <uri>. The caller is responsible for clearing the local
 * trash buffer if needed.
 */
static void stats_dump_html_head(struct appctx *appctx, struct uri_auth *uri)
{
	struct show_stat_ctx *ctx = appctx->svcctx;

	/* WARNING! This must fit in the first buffer !!! */
	chunk_appendf(&trash_chunk,
	              "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\"\n"
	              "\"http://www.w3.org/TR/html4/loose.dtd\">\n"
	              "<html><head><title>Statistics Report for " PRODUCT_NAME "%s%s</title>\n"
	              "<link rel=\"icon\" href=\"data:,\">\n"
	              "<meta http-equiv=\"content-type\" content=\"text/html; charset=iso-8859-1\">\n"
	              "<style type=\"text/css\"><!--\n"
	              "body {"
	              " font-family: arial, helvetica, sans-serif;"
	              " font-size: 12px;"
	              " font-weight: normal;"
	              " color: black;"
	              " background: white;"
	              "}\n"
	              "th,td {"
	              " font-size: 10px;"
	              "}\n"
	              "h1 {"
	              " font-size: x-large;"
	              " margin-bottom: 0.5em;"
	              "}\n"
	              "h2 {"
	              " font-family: helvetica, arial;"
	              " font-size: x-large;"
	              " font-weight: bold;"
	              " font-style: italic;"
	              " color: #6020a0;"
	              " margin-top: 0em;"
	              " margin-bottom: 0em;"
	              "}\n"
	              "h3 {"
	              " font-family: helvetica, arial;"
	              " font-size: 16px;"
	              " font-weight: bold;"
	              " color: #b00040;"
	              " background: #e8e8d0;"
	              " margin-top: 0em;"
	              " margin-bottom: 0em;"
	              "}\n"
	              "li {"
	              " margin-top: 0.25em;"
	              " margin-right: 2em;"
	              "}\n"
	              ".hr {margin-top: 0.25em;"
	              " border-color: black;"
	              " border-bottom-style: solid;"
	              "}\n"
	              ".titre	{background: #20D0D0;color: #000000; font-weight: bold; text-align: center;}\n"
	              ".total	{background: #20D0D0;color: #ffff80;}\n"
	              ".frontend	{background: #e8e8d0;}\n"
	              ".socket	{background: #d0d0d0;}\n"
	              ".backend	{background: #e8e8d0;}\n"
	              ".active_down		{background: #ff9090;}\n"
	              ".active_going_up		{background: #ffd020;}\n"
	              ".active_going_down	{background: #ffffa0;}\n"
	              ".active_up		{background: #c0ffc0;}\n"
	              ".active_nolb		{background: #20a0ff;}\n"
	              ".active_draining		{background: #20a0FF;}\n"
	              ".active_no_check		{background: #e0e0e0;}\n"
	              ".backup_down		{background: #ff9090;}\n"
	              ".backup_going_up		{background: #ff80ff;}\n"
	              ".backup_going_down	{background: #c060ff;}\n"
	              ".backup_up		{background: #b0d0ff;}\n"
	              ".backup_nolb		{background: #90b0e0;}\n"
	              ".backup_draining		{background: #cc9900;}\n"
	              ".backup_no_check		{background: #e0e0e0;}\n"
	              ".maintain	{background: #c07820;}\n"
	              ".rls      {letter-spacing: 0.2em; margin-right: 1px;}\n" /* right letter spacing (used for grouping digits) */
	              "\n"
	              "a.px:link {color: #ffff40; text-decoration: none;}"
	              "a.px:visited {color: #ffff40; text-decoration: none;}"
	              "a.px:hover {color: #ffffff; text-decoration: none;}"
	              "a.lfsb:link {color: #000000; text-decoration: none;}"
	              "a.lfsb:visited {color: #000000; text-decoration: none;}"
	              "a.lfsb:hover {color: #505050; text-decoration: none;}"
	              "\n"
	              "table.tbl { border-collapse: collapse; border-style: none;}\n"
	              "table.tbl td { text-align: right; border-width: 1px 1px 1px 1px; border-style: solid solid solid solid; padding: 2px 3px; border-color: gray; white-space: nowrap;}\n"
	              "table.tbl td.ac { text-align: center;}\n"
	              "table.tbl th { border-width: 1px; border-style: solid solid solid solid; border-color: gray;}\n"
	              "table.tbl th.pxname { background: #b00040; color: #ffff40; font-weight: bold; border-style: solid solid none solid; padding: 2px 3px; white-space: nowrap;}\n"
	              "table.tbl th.empty { border-style: none; empty-cells: hide; background: white;}\n"
	              "table.tbl th.desc { background: white; border-style: solid solid none solid; text-align: left; padding: 2px 3px;}\n"
	              "\n"
	              "table.lgd { border-collapse: collapse; border-width: 1px; border-style: none none none solid; border-color: black;}\n"
	              "table.lgd td { border-width: 1px; border-style: solid solid solid solid; border-color: gray; padding: 2px;}\n"
	              "table.lgd td.noborder { border-style: none; padding: 2px; white-space: nowrap;}\n"
	              "table.det { border-collapse: collapse; border-style: none; }\n"
	              "table.det th { text-align: left; border-width: 0px; padding: 0px 1px 0px 0px; font-style:normal;font-size:11px;font-weight:bold;font-family: sans-serif;}\n"
	              "table.det td { text-align: right; border-width: 0px; padding: 0px 0px 0px 4px; white-space: nowrap; font-style:normal;font-size:11px;font-weight:normal;}\n"
	              "u {text-decoration:none; border-bottom: 1px dotted black;}\n"
	              "div.tips {\n"
	              " display:block;\n"
	              " visibility:hidden;\n"
	              " z-index:2147483647;\n"
	              " position:absolute;\n"
	              " padding:2px 4px 3px;\n"
	              " background:#f0f060; color:#000000;\n"
	              " border:1px solid #7040c0;\n"
	              " white-space:nowrap;\n"
	              " font-style:normal;font-size:11px;font-weight:normal;\n"
	              " -moz-border-radius:3px;-webkit-border-radius:3px;border-radius:3px;\n"
	              " -moz-box-shadow:gray 2px 2px 3px;-webkit-box-shadow:gray 2px 2px 3px;box-shadow:gray 2px 2px 3px;\n"
	              "}\n"
	              "u:hover div.tips {visibility:visible;}\n"
	              "@media (prefers-color-scheme: dark) {\n"
	              " body { font-family: arial, helvetica, sans-serif; font-size: 12px; font-weight: normal; color: #e8e6e3; background: #131516;}\n"
	              " h1 { color: #a265e0!important; }\n"
	              " h2 { color: #a265e0; }\n"
	              " h3 { color: #ff5190; background-color: #3e3e1f; }\n"
	              " a { color: #3391ff; }\n"
	              " input { background-color: #2f3437; }\n"
	              " .hr { border-color: #8c8273; }\n"
	              " .titre { background-color: #1aa6a6; color: #e8e6e3; }\n"
	              " .frontend {background: #2f3437;}\n"
	              " .socket	{background: #2a2d2f;}\n"
	              " .backend {background: #2f3437;}\n"
	              " .active_down {background: #760000;}\n"
	              " .active_going_up {background: #b99200;}\n"
	              " .active_going_down {background: #6c6c00;}\n"
	              " .active_up {background: #165900;}\n"
	              " .active_nolb {background: #006ab9;}\n"
	              " .active_draining {background: #006ab9;}\n"
	              " .active_no_check {background: #2a2d2f;}\n"
	              " .backup_down {background: #760000;}\n"
	              " .backup_going_up {background: #7f007f;}\n"
	              " .backup_going_down {background: #580092;}\n"
	              " .backup_up {background: #2e3234;}\n"
	              " .backup_nolb {background: #1e3c6a;}\n"
	              " .backup_draining {background: #a37a00;}\n"
	              " .backup_no_check {background: #2a2d2f;}\n"
	              " .maintain {background: #9a601a;}\n"
	              " a.px:link {color: #d8d83b; text-decoration: none;}\n"
	              " a.px:visited {color: #d8d83b; text-decoration: none;}\n"
	              " a.px:hover {color: #ffffff; text-decoration: none;}\n"
	              " a.lfsb:link {color: #e8e6e3; text-decoration: none;}\n"
	              " a.lfsb:visited {color: #e8e6e3; text-decoration: none;}\n"
	              " a.lfsb:hover {color: #b5afa6; text-decoration: none;}\n"
	              " table.tbl th.empty { background-color: #181a1b; }\n"
	              " table.tbl th.desc { background: #181a1b; }\n"
	              " table.tbl th.pxname { background-color: #8d0033; color: #ffff46; }\n"
	              " table.tbl th { border-color: #808080; }\n"
	              " table.tbl td { border-color: #808080; }\n"
	              " u {text-decoration:none; border-bottom: 1px dotted #e8e6e3;}\n"
	              " div.tips {\n"
	              "  background:#8e8e0d;\n"
	              "  color:#e8e6e3;\n"
	              "  border-color: #4e2c86;\n"
	              "  -moz-box-shadow: #60686c 2px 2px 3px;\n"
	              "  -webkit-box-shadow: #60686c 2px 2px 3px;\n"
	              "  box-shadow: #60686c 2px 2px 3px;\n"
	              " }\n"
	              "}\n"
	              "-->\n"
	              "</style></head>\n",
	              (ctx->flags & STAT_SHNODE) ? " on " : "",
	              (ctx->flags & STAT_SHNODE) ? (uri && uri->node ? uri->node : global.node) : ""
	              );
}

/* Dumps the HTML stats information block to the local trash buffer for and uses
 * the state from stream connector <sc> and per-uri parameters <uri>. The caller
 * is responsible for clearing the local trash buffer if needed.
 */
static void stats_dump_html_info(struct stconn *sc, struct uri_auth *uri)
{
	struct appctx *appctx = __sc_appctx(sc);
	struct show_stat_ctx *ctx = appctx->svcctx;
	unsigned int up = ns_to_sec(now_ns - start_time_ns);
	char scope_txt[STAT_SCOPE_TXT_MAXLEN + sizeof STAT_SCOPE_PATTERN];
	const char *scope_ptr = stats_scope_ptr(appctx, sc);
	unsigned long long bps;
	int thr;

	for (bps = thr = 0; thr < global.nbthread; thr++)
		bps += 32ULL * read_freq_ctr(&ha_thread_ctx[thr].out_32bps);

	/* Turn the bytes per second to bits per second and take care of the
	 * usual ethernet overhead in order to help figure how far we are from
	 * interface saturation since it's the only case which usually matters.
	 * For this we count the total size of an Ethernet frame on the wire
	 * including preamble and IFG (1538) for the largest TCP segment it
	 * transports (1448 with TCP timestamps). This is not valid for smaller
	 * packets (under-estimated), but it gives a reasonably accurate
	 * estimation of how far we are from uplink saturation.
	 */
	bps = bps * 8 * 1538 / 1448;

	/* WARNING! this has to fit the first packet too.
	 * We are around 3.5 kB, add adding entries will
	 * become tricky if we want to support 4kB buffers !
	 */
	chunk_appendf(&trash_chunk,
	              "<body><h1><a href=\"" PRODUCT_URL "\" style=\"text-decoration: none;\">"
	              PRODUCT_NAME "%s</a></h1>\n"
	              "<h2>Statistics Report for pid %d%s%s%s%s</h2>\n"
	              "<hr width=\"100%%\" class=\"hr\">\n"
	              "<h3>&gt; General process information</h3>\n"
	              "<table border=0><tr><td align=\"left\" nowrap width=\"1%%\">\n"
	              "<p><b>pid = </b> %d (process #%d, nbproc = %d, nbthread = %d)<br>\n"
	              "<b>uptime = </b> %dd %dh%02dm%02ds; warnings = %u<br>\n"
	              "<b>system limits:</b> memmax = %s%s; ulimit-n = %d<br>\n"
	              "<b>maxsock = </b> %d; <b>maxconn = </b> %d; <b>reached = </b> %llu; <b>maxpipes = </b> %d<br>\n"
	              "current conns = %d; current pipes = %d/%d; conn rate = %d/sec; bit rate = %.3f %cbps<br>\n"
	              "Running tasks: %d/%d; idle = %d %%<br>\n"
	              "</td><td align=\"center\" nowrap>\n"
	              "<table class=\"lgd\"><tr>\n"
	              "<td class=\"active_up\">&nbsp;</td><td class=\"noborder\">active UP </td>"
	              "<td class=\"backup_up\">&nbsp;</td><td class=\"noborder\">backup UP </td>"
	              "</tr><tr>\n"
	              "<td class=\"active_going_down\"></td><td class=\"noborder\">active UP, going down </td>"
	              "<td class=\"backup_going_down\"></td><td class=\"noborder\">backup UP, going down </td>"
	              "</tr><tr>\n"
	              "<td class=\"active_going_up\"></td><td class=\"noborder\">active DOWN, going up </td>"
	              "<td class=\"backup_going_up\"></td><td class=\"noborder\">backup DOWN, going up </td>"
	              "</tr><tr>\n"
	              "<td class=\"active_down\"></td><td class=\"noborder\">active or backup DOWN &nbsp;</td>"
	              "<td class=\"active_no_check\"></td><td class=\"noborder\">not checked </td>"
	              "</tr><tr>\n"
	              "<td class=\"maintain\"></td><td class=\"noborder\" colspan=\"3\">active or backup DOWN for maintenance (MAINT) &nbsp;</td>"
	              "</tr><tr>\n"
	              "<td class=\"active_draining\"></td><td class=\"noborder\" colspan=\"3\">active or backup SOFT STOPPED for maintenance &nbsp;</td>"
	              "</tr></table>\n"
	              "Note: \"NOLB\"/\"DRAIN\" = UP with load-balancing disabled."
	              "</td>"
	              "<td align=\"left\" valign=\"top\" nowrap width=\"1%%\">"
	              "<b>Display option:</b><ul style=\"margin-top: 0.25em;\">"
	              "",
	              (ctx->flags & STAT_HIDEVER) ? "" : (stats_version_string),
	              pid, (ctx->flags & STAT_SHNODE) ? " on " : "",
		      (ctx->flags & STAT_SHNODE) ? (uri->node ? uri->node : global.node) : "",
	              (ctx->flags & STAT_SHDESC) ? ": " : "",
		      (ctx->flags & STAT_SHDESC) ? (uri->desc ? uri->desc : global.desc) : "",
	              pid, 1, 1, global.nbthread,
	              up / 86400, (up % 86400) / 3600,
	              (up % 3600) / 60, (up % 60),
	              HA_ATOMIC_LOAD(&tot_warnings),
	              global.rlimit_memmax ? ultoa(global.rlimit_memmax) : "unlimited",
	              global.rlimit_memmax ? " MB" : "",
	              global.rlimit_nofile,
	              global.maxsock, global.maxconn, HA_ATOMIC_LOAD(&maxconn_reached), global.maxpipes,
	              actconn, pipes_used, pipes_used+pipes_free, read_freq_ctr(&global.conn_per_sec),
		      bps >= 1000000000UL ? (bps / 1000000000.0) : bps >= 1000000UL ? (bps / 1000000.0) : (bps / 1000.0),
		      bps >= 1000000000UL ? 'G' : bps >= 1000000UL ? 'M' : 'k',
	              total_run_queues(), total_allocated_tasks(), clock_report_idle()
	              );

	/* scope_txt = search query, ctx->scope_len is always <= STAT_SCOPE_TXT_MAXLEN */
	memcpy(scope_txt, scope_ptr, ctx->scope_len);
	scope_txt[ctx->scope_len] = '\0';

	chunk_appendf(&trash_chunk,
		      "<li><form method=\"GET\">Scope : <input value=\"%s\" name=\"" STAT_SCOPE_INPUT_NAME "\" size=\"8\" maxlength=\"%d\" tabindex=\"1\"/></form>\n",
		      (ctx->scope_len > 0) ? scope_txt : "",
		      STAT_SCOPE_TXT_MAXLEN);

	/* scope_txt = search pattern + search query, ctx->scope_len is always <= STAT_SCOPE_TXT_MAXLEN */
	scope_txt[0] = 0;
	if (ctx->scope_len) {
		strlcpy2(scope_txt, STAT_SCOPE_PATTERN, sizeof(scope_txt));
		memcpy(scope_txt + strlen(STAT_SCOPE_PATTERN), scope_ptr, ctx->scope_len);
		scope_txt[strlen(STAT_SCOPE_PATTERN) + ctx->scope_len] = 0;
	}

	if (ctx->flags & STAT_HIDE_DOWN)
		chunk_appendf(&trash_chunk,
		              "<li><a href=\"%s%s%s%s\">Show all servers</a><br>\n",
		              uri->uri_prefix,
		              "",
		              (ctx->flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			      scope_txt);
	else
		chunk_appendf(&trash_chunk,
		              "<li><a href=\"%s%s%s%s\">Hide 'DOWN' servers</a><br>\n",
		              uri->uri_prefix,
		              ";up",
		              (ctx->flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			      scope_txt);

	if (uri->refresh > 0) {
		if (ctx->flags & STAT_NO_REFRESH)
			chunk_appendf(&trash_chunk,
			              "<li><a href=\"%s%s%s%s\">Enable refresh</a><br>\n",
			              uri->uri_prefix,
			              (ctx->flags & STAT_HIDE_DOWN) ? ";up" : "",
			              "",
				      scope_txt);
		else
			chunk_appendf(&trash_chunk,
			              "<li><a href=\"%s%s%s%s\">Disable refresh</a><br>\n",
			              uri->uri_prefix,
			              (ctx->flags & STAT_HIDE_DOWN) ? ";up" : "",
			              ";norefresh",
				      scope_txt);
	}

	chunk_appendf(&trash_chunk,
	              "<li><a href=\"%s%s%s%s\">Refresh now</a><br>\n",
	              uri->uri_prefix,
	              (ctx->flags & STAT_HIDE_DOWN) ? ";up" : "",
	              (ctx->flags & STAT_NO_REFRESH) ? ";norefresh" : "",
		      scope_txt);

	chunk_appendf(&trash_chunk,
	              "<li><a href=\"%s;csv%s%s\">CSV export</a><br>\n",
	              uri->uri_prefix,
	              (uri->refresh > 0) ? ";norefresh" : "",
		      scope_txt);

	chunk_appendf(&trash_chunk,
	              "<li><a href=\"%s;json%s%s\">JSON export</a> (<a href=\"%s;json-schema\">schema</a>)<br>\n",
	              uri->uri_prefix,
	              (uri->refresh > 0) ? ";norefresh" : "",
		      scope_txt, uri->uri_prefix);

	chunk_appendf(&trash_chunk,
	              "</ul></td>"
	              "<td align=\"left\" valign=\"top\" nowrap width=\"1%%\">"
	              "<b>External resources:</b><ul style=\"margin-top: 0.25em;\">\n"
	              "<li><a href=\"" PRODUCT_URL "\">Primary site</a><br>\n"
	              "<li><a href=\"" PRODUCT_URL_UPD "\">Updates (v" PRODUCT_BRANCH ")</a><br>\n"
	              "<li><a href=\"" PRODUCT_URL_DOC "\">Online manual</a><br>\n"
	              "</ul>"
	              "</td>"
	              "</tr></table>\n"
	              ""
	              );

	if (ctx->st_code) {
		switch (ctx->st_code) {
		case STAT_STATUS_DONE:
			chunk_appendf(&trash_chunk,
			              "<p><div class=active_up>"
			              "<a class=lfsb href=\"%s%s%s%s\" title=\"Remove this message\">[X]</a> "
			              "Action processed successfully."
			              "</div>\n", uri->uri_prefix,
			              (ctx->flags & STAT_HIDE_DOWN) ? ";up" : "",
			              (ctx->flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			              scope_txt);
			break;
		case STAT_STATUS_NONE:
			chunk_appendf(&trash_chunk,
			              "<p><div class=active_going_down>"
			              "<a class=lfsb href=\"%s%s%s%s\" title=\"Remove this message\">[X]</a> "
			              "Nothing has changed."
			              "</div>\n", uri->uri_prefix,
			              (ctx->flags & STAT_HIDE_DOWN) ? ";up" : "",
			              (ctx->flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			              scope_txt);
			break;
		case STAT_STATUS_PART:
			chunk_appendf(&trash_chunk,
			              "<p><div class=active_going_down>"
			              "<a class=lfsb href=\"%s%s%s%s\" title=\"Remove this message\">[X]</a> "
			              "Action partially processed.<br>"
			              "Some server names are probably unknown or ambiguous (duplicated names in the backend)."
			              "</div>\n", uri->uri_prefix,
			              (ctx->flags & STAT_HIDE_DOWN) ? ";up" : "",
			              (ctx->flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			              scope_txt);
			break;
		case STAT_STATUS_ERRP:
			chunk_appendf(&trash_chunk,
			              "<p><div class=active_down>"
			              "<a class=lfsb href=\"%s%s%s%s\" title=\"Remove this message\">[X]</a> "
			              "Action not processed because of invalid parameters."
			              "<ul>"
			              "<li>The action is maybe unknown.</li>"
				      "<li>Invalid key parameter (empty or too long).</li>"
			              "<li>The backend name is probably unknown or ambiguous (duplicated names).</li>"
			              "<li>Some server names are probably unknown or ambiguous (duplicated names in the backend).</li>"
			              "</ul>"
			              "</div>\n", uri->uri_prefix,
			              (ctx->flags & STAT_HIDE_DOWN) ? ";up" : "",
			              (ctx->flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			              scope_txt);
			break;
		case STAT_STATUS_EXCD:
			chunk_appendf(&trash_chunk,
			              "<p><div class=active_down>"
			              "<a class=lfsb href=\"%s%s%s%s\" title=\"Remove this message\">[X]</a> "
			              "<b>Action not processed : the buffer couldn't store all the data.<br>"
			              "You should retry with less servers at a time.</b>"
			              "</div>\n", uri->uri_prefix,
			              (ctx->flags & STAT_HIDE_DOWN) ? ";up" : "",
			              (ctx->flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			              scope_txt);
			break;
		case STAT_STATUS_DENY:
			chunk_appendf(&trash_chunk,
			              "<p><div class=active_down>"
			              "<a class=lfsb href=\"%s%s%s%s\" title=\"Remove this message\">[X]</a> "
			              "<b>Action denied.</b>"
			              "</div>\n", uri->uri_prefix,
			              (ctx->flags & STAT_HIDE_DOWN) ? ";up" : "",
			              (ctx->flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			              scope_txt);
			break;
		case STAT_STATUS_IVAL:
			chunk_appendf(&trash_chunk,
			              "<p><div class=active_down>"
			              "<a class=lfsb href=\"%s%s%s%s\" title=\"Remove this message\">[X]</a> "
			              "<b>Invalid requests (unsupported method or chunked encoded request).</b>"
			              "</div>\n", uri->uri_prefix,
			              (ctx->flags & STAT_HIDE_DOWN) ? ";up" : "",
			              (ctx->flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			              scope_txt);
			break;
		default:
			chunk_appendf(&trash_chunk,
			              "<p><div class=active_no_check>"
			              "<a class=lfsb href=\"%s%s%s%s\" title=\"Remove this message\">[X]</a> "
			              "Unexpected result."
			              "</div>\n", uri->uri_prefix,
			              (ctx->flags & STAT_HIDE_DOWN) ? ";up" : "",
			              (ctx->flags & STAT_NO_REFRESH) ? ";norefresh" : "",
			              scope_txt);
		}
		chunk_appendf(&trash_chunk, "<p>\n");
	}
}

/* Dumps the HTML stats trailer block to the local trash buffer. The caller is
 * responsible for clearing the local trash buffer if needed.
 */
static void stats_dump_html_end()
{
	chunk_appendf(&trash_chunk, "</body></html>\n");
}

/* Dumps the stats JSON header to the local trash buffer buffer which. The
 * caller is responsible for clearing it if needed.
 */
static void stats_dump_json_header()
{
	chunk_strcat(&trash_chunk, "[");
}


/* Dumps the JSON stats trailer block to the local trash buffer. The caller is
 * responsible for clearing the local trash buffer if needed.
 */
static void stats_dump_json_end()
{
	chunk_strcat(&trash_chunk, "]\n");
}

/* Uses <appctx.ctx.stats.obj1> as a pointer to the current proxy and <obj2> as
 * a pointer to the current server/listener.
 */
static int stats_dump_proxies(struct stconn *sc,
                              struct htx *htx,
                              struct uri_auth *uri)
{
	struct appctx *appctx = __sc_appctx(sc);
	struct show_stat_ctx *ctx = appctx->svcctx;
	struct channel *rep = sc_ic(sc);
	struct proxy *px;

	/* dump proxies */
	while (ctx->obj1) {
		if (htx) {
			if (htx_almost_full(htx)) {
				sc_need_room(sc, htx->size / 2);
				goto full;
			}
		}
		else {
			if (buffer_almost_full(&rep->buf)) {
				sc_need_room(sc, b_size(&rep->buf) / 2);
				goto full;
			}
		}

		px = ctx->obj1;
		/* Skip the global frontend proxies and non-networked ones.
		 * Also skip proxies that were disabled in the configuration
		 * This change allows retrieving stats from "old" proxies after a reload.
		 */
		if (!(px->flags & PR_FL_DISABLED) && px->uuid > 0 &&
		    (px->cap & (PR_CAP_FE | PR_CAP_BE)) && !(px->cap & PR_CAP_INT)) {
			if (stats_dump_proxy_to_buffer(sc, htx, px, uri) == 0)
				return 0;
		}

		ctx->obj1 = px->next;
		ctx->px_st = STAT_PX_ST_INIT;
		ctx->field = 0;
	}

	return 1;

  full:
	return 0;
}

/* This function dumps statistics onto the stream connector's read buffer in
 * either CSV or HTML format. <uri> contains some HTML-specific parameters that
 * are ignored for CSV format (hence <uri> may be NULL there). It returns 0 if
 * it had to stop writing data and an I/O is needed, 1 if the dump is finished
 * and the stream must be closed, or -1 in case of any error. This function is
 * used by both the CLI and the HTTP handlers.
 */
static int stats_dump_stat_to_buffer(struct stconn *sc, struct htx *htx,
				     struct uri_auth *uri)
{
	struct appctx *appctx = __sc_appctx(sc);
	struct show_stat_ctx *ctx = appctx->svcctx;
	enum stats_domain domain = ctx->domain;

	chunk_reset(&trash_chunk);

	switch (ctx->state) {
	case STAT_STATE_INIT:
		ctx->state = STAT_STATE_HEAD; /* let's start producing data */
		__fallthrough;

	case STAT_STATE_HEAD:
		if (ctx->flags & STAT_FMT_HTML)
			stats_dump_html_head(appctx, uri);
		else if (ctx->flags & STAT_JSON_SCHM)
			stats_dump_json_schema(&trash_chunk);
		else if (ctx->flags & STAT_FMT_JSON)
			stats_dump_json_header();
		else if (!(ctx->flags & STAT_FMT_TYPED))
			stats_dump_csv_header(ctx->domain);

		if (!stats_putchk(appctx, htx))
			goto full;

		if (ctx->flags & STAT_JSON_SCHM) {
			ctx->state = STAT_STATE_FIN;
			return 1;
		}
		ctx->state = STAT_STATE_INFO;
		__fallthrough;

	case STAT_STATE_INFO:
		if (ctx->flags & STAT_FMT_HTML) {
			stats_dump_html_info(sc, uri);
			if (!stats_putchk(appctx, htx))
				goto full;
		}

		if (domain == STATS_DOMAIN_PROXY)
			ctx->obj1 = proxies_list;

		ctx->px_st = STAT_PX_ST_INIT;
		ctx->field = 0;
		ctx->state = STAT_STATE_LIST;
		__fallthrough;

	case STAT_STATE_LIST:
		switch (domain) {
		case STATS_DOMAIN_RESOLVERS:
			if (!stats_dump_resolvers(sc, stat_l[domain],
			                          stat_count[domain],
			                          &stats_module_list[domain])) {
				return 0;
			}
			break;

		case STATS_DOMAIN_PROXY:
		default:
			/* dump proxies */
			if (!stats_dump_proxies(sc, htx, uri))
				return 0;
			break;
		}

		ctx->state = STAT_STATE_END;
		__fallthrough;

	case STAT_STATE_END:
		if (ctx->flags & (STAT_FMT_HTML|STAT_FMT_JSON)) {
			if (ctx->flags & STAT_FMT_HTML)
				stats_dump_html_end();
			else
				stats_dump_json_end();
			if (!stats_putchk(appctx, htx))
				goto full;
		}

		ctx->state = STAT_STATE_FIN;
		__fallthrough;

	case STAT_STATE_FIN:
		return 1;

	default:
		/* unknown state ! */
		ctx->state = STAT_STATE_FIN;
		return -1;
	}

  full:
	return 0;

}

/* We reached the stats page through a POST request. The appctx is
 * expected to have already been allocated by the caller.
 * Parse the posted data and enable/disable servers if necessary.
 * Returns 1 if request was parsed or zero if it needs more data.
 */
static int stats_process_http_post(struct stconn *sc)
{
	struct stream *s = __sc_strm(sc);
	struct appctx *appctx = __sc_appctx(sc);
	struct show_stat_ctx *ctx = appctx->svcctx;

	struct proxy *px = NULL;
	struct server *sv = NULL;

	char key[LINESIZE];
	int action = ST_ADM_ACTION_NONE;
	int reprocess = 0;

	int total_servers = 0;
	int altered_servers = 0;

	char *first_param, *cur_param, *next_param, *end_params;
	char *st_cur_param = NULL;
	char *st_next_param = NULL;

	struct buffer *temp = get_trash_chunk();

	struct htx *htx = htxbuf(&s->req.buf);
	struct htx_blk *blk;

	/*  we need more data */
	if (s->txn->req.msg_state < HTTP_MSG_DONE) {
		/* check if we can receive more */
		if (htx_free_data_space(htx) <= global.tune.maxrewrite) {
			ctx->st_code = STAT_STATUS_EXCD;
			goto out;
		}
		goto wait;
	}

	/* The request was fully received. Copy data */
	blk = htx_get_head_blk(htx);
	while (blk) {
		enum htx_blk_type type = htx_get_blk_type(blk);

		if (type == HTX_BLK_TLR || type == HTX_BLK_EOT)
			break;
		if (type == HTX_BLK_DATA) {
			struct ist v = htx_get_blk_value(htx, blk);

			if (!chunk_memcat(temp, v.ptr, v.len)) {
				ctx->st_code = STAT_STATUS_EXCD;
				goto out;
			}
		}
		blk = htx_get_next_blk(htx, blk);
	}

	first_param = temp->area;
	end_params  = temp->area + temp->data;
	cur_param = next_param = end_params;
	*end_params = '\0';

	ctx->st_code = STAT_STATUS_NONE;

	/*
	 * Parse the parameters in reverse order to only store the last value.
	 * From the html form, the backend and the action are at the end.
	 */
	while (cur_param > first_param) {
		char *value;
		int poffset, plen;

		cur_param--;

		if ((*cur_param == '&') || (cur_param == first_param)) {
		reprocess_servers:
			/* Parse the key */
			poffset = (cur_param != first_param ? 1 : 0);
			plen = next_param - cur_param + (cur_param == first_param ? 1 : 0);
			if ((plen > 0) && (plen <= sizeof(key))) {
				strncpy(key, cur_param + poffset, plen);
				key[plen - 1] = '\0';
			} else {
				ctx->st_code = STAT_STATUS_ERRP;
				goto out;
			}

			/* Parse the value */
			value = key;
			while (*value != '\0' && *value != '=') {
				value++;
			}
			if (*value == '=') {
				/* Ok, a value is found, we can mark the end of the key */
				*value++ = '\0';
			}
			if (url_decode(key, 1) < 0 || url_decode(value, 1) < 0)
				break;

			/* Now we can check the key to see what to do */
			if (!px && (strcmp(key, "b") == 0)) {
				if ((px = proxy_be_by_name(value)) == NULL) {
					/* the backend name is unknown or ambiguous (duplicate names) */
					ctx->st_code = STAT_STATUS_ERRP;
					goto out;
				}
			}
			else if (!action && (strcmp(key, "action") == 0)) {
				if (strcmp(value, "ready") == 0) {
					action = ST_ADM_ACTION_READY;
				}
				else if (strcmp(value, "drain") == 0) {
					action = ST_ADM_ACTION_DRAIN;
				}
				else if (strcmp(value, "maint") == 0) {
					action = ST_ADM_ACTION_MAINT;
				}
				else if (strcmp(value, "shutdown") == 0) {
					action = ST_ADM_ACTION_SHUTDOWN;
				}
				else if (strcmp(value, "dhlth") == 0) {
					action = ST_ADM_ACTION_DHLTH;
				}
				else if (strcmp(value, "ehlth") == 0) {
					action = ST_ADM_ACTION_EHLTH;
				}
				else if (strcmp(value, "hrunn") == 0) {
					action = ST_ADM_ACTION_HRUNN;
				}
				else if (strcmp(value, "hnolb") == 0) {
					action = ST_ADM_ACTION_HNOLB;
				}
				else if (strcmp(value, "hdown") == 0) {
					action = ST_ADM_ACTION_HDOWN;
				}
				else if (strcmp(value, "dagent") == 0) {
					action = ST_ADM_ACTION_DAGENT;
				}
				else if (strcmp(value, "eagent") == 0) {
					action = ST_ADM_ACTION_EAGENT;
				}
				else if (strcmp(value, "arunn") == 0) {
					action = ST_ADM_ACTION_ARUNN;
				}
				else if (strcmp(value, "adown") == 0) {
					action = ST_ADM_ACTION_ADOWN;
				}
				/* else these are the old supported methods */
				else if (strcmp(value, "disable") == 0) {
					action = ST_ADM_ACTION_DISABLE;
				}
				else if (strcmp(value, "enable") == 0) {
					action = ST_ADM_ACTION_ENABLE;
				}
				else if (strcmp(value, "stop") == 0) {
					action = ST_ADM_ACTION_STOP;
				}
				else if (strcmp(value, "start") == 0) {
					action = ST_ADM_ACTION_START;
				}
				else {
					ctx->st_code = STAT_STATUS_ERRP;
					goto out;
				}
			}
			else if (strcmp(key, "s") == 0) {
				if (!(px && action)) {
					/*
					 * Indicates that we'll need to reprocess the parameters
					 * as soon as backend and action are known
					 */
					if (!reprocess) {
						st_cur_param  = cur_param;
						st_next_param = next_param;
					}
					reprocess = 1;
				}
				else if ((sv = findserver(px, value)) != NULL) {
					HA_SPIN_LOCK(SERVER_LOCK, &sv->lock);
					switch (action) {
					case ST_ADM_ACTION_DISABLE:
						if (!(sv->cur_admin & SRV_ADMF_FMAINT)) {
							altered_servers++;
							total_servers++;
							srv_set_admin_flag(sv, SRV_ADMF_FMAINT, SRV_ADM_STCHGC_STATS_DISABLE);
						}
						break;
					case ST_ADM_ACTION_ENABLE:
						if (sv->cur_admin & SRV_ADMF_FMAINT) {
							altered_servers++;
							total_servers++;
							srv_clr_admin_flag(sv, SRV_ADMF_FMAINT);
						}
						break;
					case ST_ADM_ACTION_STOP:
						if (!(sv->cur_admin & SRV_ADMF_FDRAIN)) {
							srv_set_admin_flag(sv, SRV_ADMF_FDRAIN, SRV_ADM_STCHGC_STATS_STOP);
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_START:
						if (sv->cur_admin & SRV_ADMF_FDRAIN) {
							srv_clr_admin_flag(sv, SRV_ADMF_FDRAIN);
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_DHLTH:
						if (sv->check.state & CHK_ST_CONFIGURED) {
							sv->check.state &= ~CHK_ST_ENABLED;
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_EHLTH:
						if (sv->check.state & CHK_ST_CONFIGURED) {
							sv->check.state |= CHK_ST_ENABLED;
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_HRUNN:
						if (!(sv->track)) {
							sv->check.health = sv->check.rise + sv->check.fall - 1;
							srv_set_running(sv, SRV_OP_STCHGC_STATS_WEB);
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_HNOLB:
						if (!(sv->track)) {
							sv->check.health = sv->check.rise + sv->check.fall - 1;
							srv_set_stopping(sv, SRV_OP_STCHGC_STATS_WEB);
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_HDOWN:
						if (!(sv->track)) {
							sv->check.health = 0;
							srv_set_stopped(sv, SRV_OP_STCHGC_STATS_WEB);
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_DAGENT:
						if (sv->agent.state & CHK_ST_CONFIGURED) {
							sv->agent.state &= ~CHK_ST_ENABLED;
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_EAGENT:
						if (sv->agent.state & CHK_ST_CONFIGURED) {
							sv->agent.state |= CHK_ST_ENABLED;
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_ARUNN:
						if (sv->agent.state & CHK_ST_ENABLED) {
							sv->agent.health = sv->agent.rise + sv->agent.fall - 1;
							srv_set_running(sv, SRV_OP_STCHGC_STATS_WEB);
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_ADOWN:
						if (sv->agent.state & CHK_ST_ENABLED) {
							sv->agent.health = 0;
							srv_set_stopped(sv, SRV_OP_STCHGC_STATS_WEB);
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_READY:
						srv_adm_set_ready(sv);
						altered_servers++;
						total_servers++;
						break;
					case ST_ADM_ACTION_DRAIN:
						srv_adm_set_drain(sv);
						altered_servers++;
						total_servers++;
						break;
					case ST_ADM_ACTION_MAINT:
						srv_adm_set_maint(sv);
						altered_servers++;
						total_servers++;
						break;
					case ST_ADM_ACTION_SHUTDOWN:
						if (!(px->flags & (PR_FL_DISABLED|PR_FL_STOPPED))) {
							srv_shutdown_streams(sv, SF_ERR_KILLED);
							altered_servers++;
							total_servers++;
						}
						break;
					}
					HA_SPIN_UNLOCK(SERVER_LOCK, &sv->lock);
				} else {
					/* the server name is unknown or ambiguous (duplicate names) */
					total_servers++;
				}
			}
			if (reprocess && px && action) {
				/* Now, we know the backend and the action chosen by the user.
				 * We can safely restart from the first server parameter
				 * to reprocess them
				 */
				cur_param  = st_cur_param;
				next_param = st_next_param;
				reprocess = 0;
				goto reprocess_servers;
			}

			next_param = cur_param;
		}
	}

	if (total_servers == 0) {
		ctx->st_code = STAT_STATUS_NONE;
	}
	else if (altered_servers == 0) {
		ctx->st_code = STAT_STATUS_ERRP;
	}
	else if (altered_servers == total_servers) {
		ctx->st_code = STAT_STATUS_DONE;
	}
	else {
		ctx->st_code = STAT_STATUS_PART;
	}
 out:
	return 1;
 wait:
	ctx->st_code = STAT_STATUS_NONE;
	return 0;
}


static int stats_send_http_headers(struct stconn *sc, struct htx *htx)
{
	struct stream *s = __sc_strm(sc);
	struct uri_auth *uri = s->be->uri_auth;
	struct appctx *appctx = __sc_appctx(sc);
	struct show_stat_ctx *ctx = appctx->svcctx;
	struct htx_sl *sl;
	unsigned int flags;

	flags = (HTX_SL_F_IS_RESP|HTX_SL_F_VER_11|HTX_SL_F_XFER_ENC|HTX_SL_F_XFER_LEN|HTX_SL_F_CHNK);
	sl = htx_add_stline(htx, HTX_BLK_RES_SL, flags, ist("HTTP/1.1"), ist("200"), ist("OK"));
	if (!sl)
		goto full;
	sl->info.res.status = 200;

	if (!htx_add_header(htx, ist("Cache-Control"), ist("no-cache")))
		goto full;
	if (ctx->flags & STAT_FMT_HTML) {
		if (!htx_add_header(htx, ist("Content-Type"), ist("text/html")))
			goto full;
	}
	else if (ctx->flags & (STAT_FMT_JSON|STAT_JSON_SCHM)) {
		if (!htx_add_header(htx, ist("Content-Type"), ist("application/json")))
			goto full;
	}
	else {
		if (!htx_add_header(htx, ist("Content-Type"), ist("text/plain")))
			goto full;
	}

	if (uri->refresh > 0 && !(ctx->flags & STAT_NO_REFRESH)) {
		const char *refresh = U2A(uri->refresh);
		if (!htx_add_header(htx, ist("Refresh"), ist(refresh)))
			goto full;
	}

	if (ctx->flags & STAT_CHUNKED) {
		if (!htx_add_header(htx, ist("Transfer-Encoding"), ist("chunked")))
			goto full;
	}

	if (!htx_add_endof(htx, HTX_BLK_EOH))
		goto full;

	channel_add_input(&s->res, htx->data);
	return 1;

  full:
	htx_reset(htx);
	sc_need_room(sc, 0);
	return 0;
}


static int stats_send_http_redirect(struct stconn *sc, struct htx *htx)
{
	char scope_txt[STAT_SCOPE_TXT_MAXLEN + sizeof STAT_SCOPE_PATTERN];
	struct stream *s = __sc_strm(sc);
	struct uri_auth *uri = s->be->uri_auth;
	struct appctx *appctx = __sc_appctx(sc);
	struct show_stat_ctx *ctx = appctx->svcctx;
	struct htx_sl *sl;
	unsigned int flags;

	/* scope_txt = search pattern + search query, ctx->scope_len is always <= STAT_SCOPE_TXT_MAXLEN */
	scope_txt[0] = 0;
	if (ctx->scope_len) {
		const char *scope_ptr = stats_scope_ptr(appctx, sc);

		strlcpy2(scope_txt, STAT_SCOPE_PATTERN, sizeof(scope_txt));
		memcpy(scope_txt + strlen(STAT_SCOPE_PATTERN), scope_ptr, ctx->scope_len);
		scope_txt[strlen(STAT_SCOPE_PATTERN) + ctx->scope_len] = 0;
	}

	/* We don't want to land on the posted stats page because a refresh will
	 * repost the data. We don't want this to happen on accident so we redirect
	 * the browse to the stats page with a GET.
	 */
	chunk_printf(&trash, "%s;st=%s%s%s%s",
		     uri->uri_prefix,
		     ((ctx->st_code > STAT_STATUS_INIT) &&
		      (ctx->st_code < STAT_STATUS_SIZE) &&
		      stat_status_codes[ctx->st_code]) ?
		     stat_status_codes[ctx->st_code] :
		     stat_status_codes[STAT_STATUS_UNKN],
		     (ctx->flags & STAT_HIDE_DOWN) ? ";up" : "",
		     (ctx->flags & STAT_NO_REFRESH) ? ";norefresh" : "",
		     scope_txt);

	flags = (HTX_SL_F_IS_RESP|HTX_SL_F_VER_11|HTX_SL_F_XFER_LEN|HTX_SL_F_CHNK);
	sl = htx_add_stline(htx, HTX_BLK_RES_SL, flags, ist("HTTP/1.1"), ist("303"), ist("See Other"));
	if (!sl)
		goto full;
	sl->info.res.status = 303;

	if (!htx_add_header(htx, ist("Cache-Control"), ist("no-cache")) ||
	    !htx_add_header(htx, ist("Content-Type"), ist("text/plain")) ||
	    !htx_add_header(htx, ist("Content-Length"), ist("0")) ||
	    !htx_add_header(htx, ist("Location"), ist2(trash.area, trash.data)))
		goto full;

	if (!htx_add_endof(htx, HTX_BLK_EOH))
		goto full;

	channel_add_input(&s->res, htx->data);
	return 1;

full:
	htx_reset(htx);
	sc_need_room(sc, 0);
	return 0;
}


/* This I/O handler runs as an applet embedded in a stream connector. It is
 * used to send HTTP stats over a TCP socket. The mechanism is very simple.
 * appctx->st0 contains the operation in progress (dump, done). The handler
 * automatically unregisters itself once transfer is complete.
 */
static void http_stats_io_handler(struct appctx *appctx)
{
	struct show_stat_ctx *ctx = appctx->svcctx;
	struct stconn *sc = appctx_sc(appctx);
	struct stream *s = __sc_strm(sc);
	struct channel *req = sc_oc(sc);
	struct channel *res = sc_ic(sc);
	struct htx *req_htx, *res_htx;

	/* only proxy stats are available via http */
	ctx->domain = STATS_DOMAIN_PROXY;

	res_htx = htx_from_buf(&res->buf);

	if (unlikely(se_fl_test(appctx->sedesc, (SE_FL_EOS|SE_FL_ERROR|SE_FL_SHR|SE_FL_SHW)))) {
		appctx->st0 = STAT_HTTP_END;
		goto out;
	}

	/* Check if the input buffer is available. */
	if (!b_size(&res->buf)) {
		sc_need_room(sc, 0);
		goto out;
	}

	/* all states are processed in sequence */
	if (appctx->st0 == STAT_HTTP_HEAD) {
		if (stats_send_http_headers(sc, res_htx)) {
			if (s->txn->meth == HTTP_METH_HEAD)
				appctx->st0 = STAT_HTTP_DONE;
			else
				appctx->st0 = STAT_HTTP_DUMP;
		}
	}

	if (appctx->st0 == STAT_HTTP_DUMP) {
		trash_chunk = b_make(trash.area, res->buf.size, 0, 0);
		/* adjust buffer size to take htx overhead into account,
		 * make sure to perform this call on an empty buffer
		 */
		trash_chunk.size = buf_room_for_htx_data(&trash_chunk);
		if (stats_dump_stat_to_buffer(sc, res_htx, s->be->uri_auth))
			appctx->st0 = STAT_HTTP_DONE;
	}

	if (appctx->st0 == STAT_HTTP_POST) {
		if (stats_process_http_post(sc))
			appctx->st0 = STAT_HTTP_LAST;
		else if (s->scf->flags & (SC_FL_EOS|SC_FL_ABRT_DONE))
			appctx->st0 = STAT_HTTP_DONE;
	}

	if (appctx->st0 == STAT_HTTP_LAST) {
		if (stats_send_http_redirect(sc, res_htx))
			appctx->st0 = STAT_HTTP_DONE;
	}

	if (appctx->st0 == STAT_HTTP_DONE) {
		/* no more data are expected. If the response buffer is empty,
		 * be sure to add something (EOT block in this case) to have
		 * something to send. It is important to be sure the EOM flags
		 * will be handled by the endpoint.
		 */
		if (htx_is_empty(res_htx)) {
			if (!htx_add_endof(res_htx, HTX_BLK_EOT)) {
				sc_need_room(sc, sizeof(struct htx_blk) + 1);
				goto out;
			}
			channel_add_input(res, 1);
		}
		res_htx->flags |= HTX_FL_EOM;
		se_fl_set(appctx->sedesc, SE_FL_EOI);
		appctx->st0 = STAT_HTTP_END;
	}

	if (appctx->st0 == STAT_HTTP_END) {
		se_fl_set(appctx->sedesc, SE_FL_EOS);
		applet_will_consume(appctx);
	}

 out:
	/* we have left the request in the buffer for the case where we
	 * process a POST, and this automatically re-enables activity on
	 * read. It's better to indicate that we want to stop reading when
	 * we're sending, so that we know there's at most one direction
	 * deciding to wake the applet up. It saves it from looping when
	 * emitting large blocks into small TCP windows.
	 */
	htx_to_buf(res_htx, &res->buf);
	if (appctx->st0 == STAT_HTTP_END) {
		/* eat the whole request */
		if (co_data(req)) {
			req_htx = htx_from_buf(&req->buf);
			co_htx_skip(req, req_htx, co_data(req));
			htx_to_buf(req_htx, &req->buf);
		}
	}
	else if (!channel_is_empty(res))
		applet_wont_consume(appctx);
}

/* Dump all fields from <info> into <out> using the "show info" format (name: value) */
static int stats_dump_info_fields(struct buffer *out,
                                  const struct field *info,
                                  struct show_stat_ctx *ctx)
{
	int flags = ctx->flags;
	int field;

	for (field = 0; field < INF_TOTAL_FIELDS; field++) {
		if (!field_format(info, field))
			continue;

		if (!chunk_appendf(out, "%s: ", info_fields[field].name))
			return 0;
		if (!stats_emit_raw_data_field(out, &info[field]))
			return 0;
		if ((flags & STAT_SHOW_FDESC) && !chunk_appendf(out, ":\"%s\"", info_fields[field].desc))
			return 0;
		if (!chunk_strcat(out, "\n"))
			return 0;
	}
	return 1;
}

/* Dump all fields from <info> into <out> using the "show info typed" format */
static int stats_dump_typed_info_fields(struct buffer *out,
                                        const struct field *info,
                                        struct show_stat_ctx *ctx)
{
	int flags = ctx->flags;
	int field;

	for (field = 0; field < INF_TOTAL_FIELDS; field++) {
		if (!field_format(info, field))
			continue;

		if (!chunk_appendf(out, "%d.%s.%u:", field, info_fields[field].name, info[INF_PROCESS_NUM].u.u32))
			return 0;
		if (!stats_emit_field_tags(out, &info[field], ':'))
			return 0;
		if (!stats_emit_typed_data_field(out, &info[field]))
			return 0;
		if ((flags & STAT_SHOW_FDESC) && !chunk_appendf(out, ":\"%s\"", info_fields[field].desc))
			return 0;
		if (!chunk_strcat(out, "\n"))
			return 0;
	}
	return 1;
}

/* Fill <info> with HAProxy global info. <info> is preallocated array of length
 * <len>. The length of the array must be INF_TOTAL_FIELDS. If this length is
 * less then this value, the function returns 0, otherwise, it returns 1. Some
 * fields' presence or precision may depend on some of the STAT_* flags present
 * in <flags>.
 */
int stats_fill_info(struct field *info, int len, uint flags)
{
	struct buffer *out = get_trash_chunk();
	uint64_t glob_out_bytes, glob_spl_bytes, glob_out_b32;
	uint up_sec, up_usec;
	ullong up;
	ulong boot;
	int thr;

#ifdef USE_OPENSSL
	double ssl_sess_rate = read_freq_ctr_flt(&global.ssl_per_sec);
	double ssl_key_rate  = read_freq_ctr_flt(&global.ssl_fe_keys_per_sec);
	double ssl_reuse = 0;

	if (ssl_key_rate < ssl_sess_rate)
		ssl_reuse = 100.0 * (1.0 - ssl_key_rate / ssl_sess_rate);
#endif

	/* sum certain per-thread totals (mostly byte counts) */
	glob_out_bytes = glob_spl_bytes = glob_out_b32 = 0;
	for (thr = 0; thr < global.nbthread; thr++) {
		glob_out_bytes += HA_ATOMIC_LOAD(&ha_thread_ctx[thr].out_bytes);
		glob_spl_bytes += HA_ATOMIC_LOAD(&ha_thread_ctx[thr].spliced_out_bytes);
		glob_out_b32   += read_freq_ctr(&ha_thread_ctx[thr].out_32bps);
	}
	glob_out_b32 *= 32; // values are 32-byte units

	up = now_ns - start_time_ns;
	up_sec = ns_to_sec(up);
	up_usec = (up / 1000U) % 1000000U;

	boot = tv_ms_remain(&start_date, &ready_date);

	if (len < INF_TOTAL_FIELDS)
		return 0;

	chunk_reset(out);
	memset(info, 0, sizeof(*info) * len);

	info[INF_NAME]                           = mkf_str(FO_PRODUCT|FN_OUTPUT|FS_SERVICE, PRODUCT_NAME);
	info[INF_VERSION]                        = mkf_str(FO_PRODUCT|FN_OUTPUT|FS_SERVICE, haproxy_version);
	info[INF_BUILD_INFO]                     = mkf_str(FO_PRODUCT|FN_OUTPUT|FS_SERVICE, haproxy_version);
	info[INF_RELEASE_DATE]                   = mkf_str(FO_PRODUCT|FN_OUTPUT|FS_SERVICE, haproxy_date);

	info[INF_NBTHREAD]                       = mkf_u32(FO_CONFIG|FS_SERVICE, global.nbthread);
	info[INF_NBPROC]                         = mkf_u32(FO_CONFIG|FS_SERVICE, 1);
	info[INF_PROCESS_NUM]                    = mkf_u32(FO_KEY, 1);
	info[INF_PID]                            = mkf_u32(FO_STATUS, pid);

	info[INF_UPTIME]                         = mkf_str(FN_DURATION, chunk_newstr(out));
	chunk_appendf(out, "%ud %uh%02um%02us", up_sec / 86400, (up_sec % 86400) / 3600, (up_sec % 3600) / 60, (up_sec % 60));

	info[INF_UPTIME_SEC]                     = (flags & STAT_USE_FLOAT) ? mkf_flt(FN_DURATION, up_sec + up_usec / 1000000.0) : mkf_u32(FN_DURATION, up_sec);
	info[INF_START_TIME_SEC]                 = (flags & STAT_USE_FLOAT) ? mkf_flt(FN_DURATION, start_date.tv_sec + start_date.tv_usec / 1000000.0) : mkf_u32(FN_DURATION, start_date.tv_sec);
	info[INF_MEMMAX_MB]                      = mkf_u32(FO_CONFIG|FN_LIMIT, global.rlimit_memmax);
	info[INF_MEMMAX_BYTES]                   = mkf_u32(FO_CONFIG|FN_LIMIT, global.rlimit_memmax * 1048576L);
	info[INF_POOL_ALLOC_MB]                  = mkf_u32(0, (unsigned)(pool_total_allocated() / 1048576L));
	info[INF_POOL_ALLOC_BYTES]               = mkf_u64(0, pool_total_allocated());
	info[INF_POOL_USED_MB]                   = mkf_u32(0, (unsigned)(pool_total_used() / 1048576L));
	info[INF_POOL_USED_BYTES]                = mkf_u64(0, pool_total_used());
	info[INF_POOL_FAILED]                    = mkf_u32(FN_COUNTER, pool_total_failures());
	info[INF_ULIMIT_N]                       = mkf_u32(FO_CONFIG|FN_LIMIT, global.rlimit_nofile);
	info[INF_MAXSOCK]                        = mkf_u32(FO_CONFIG|FN_LIMIT, global.maxsock);
	info[INF_MAXCONN]                        = mkf_u32(FO_CONFIG|FN_LIMIT, global.maxconn);
	info[INF_HARD_MAXCONN]                   = mkf_u32(FO_CONFIG|FN_LIMIT, global.hardmaxconn);
	info[INF_CURR_CONN]                      = mkf_u32(0, actconn);
	info[INF_CUM_CONN]                       = mkf_u32(FN_COUNTER, totalconn);
	info[INF_CUM_REQ]                        = mkf_u32(FN_COUNTER, global.req_count);
#ifdef USE_OPENSSL
	info[INF_MAX_SSL_CONNS]                  = mkf_u32(FN_MAX, global.maxsslconn);
	info[INF_CURR_SSL_CONNS]                 = mkf_u32(0, global.sslconns);
	info[INF_CUM_SSL_CONNS]                  = mkf_u32(FN_COUNTER, global.totalsslconns);
#endif
	info[INF_MAXPIPES]                       = mkf_u32(FO_CONFIG|FN_LIMIT, global.maxpipes);
	info[INF_PIPES_USED]                     = mkf_u32(0, pipes_used);
	info[INF_PIPES_FREE]                     = mkf_u32(0, pipes_free);
	info[INF_CONN_RATE]                      = (flags & STAT_USE_FLOAT) ? mkf_flt(FN_RATE, read_freq_ctr_flt(&global.conn_per_sec)) : mkf_u32(FN_RATE, read_freq_ctr(&global.conn_per_sec));
	info[INF_CONN_RATE_LIMIT]                = mkf_u32(FO_CONFIG|FN_LIMIT, global.cps_lim);
	info[INF_MAX_CONN_RATE]                  = mkf_u32(FN_MAX, global.cps_max);
	info[INF_SESS_RATE]                      = (flags & STAT_USE_FLOAT) ? mkf_flt(FN_RATE, read_freq_ctr_flt(&global.sess_per_sec)) : mkf_u32(FN_RATE, read_freq_ctr(&global.sess_per_sec));
	info[INF_SESS_RATE_LIMIT]                = mkf_u32(FO_CONFIG|FN_LIMIT, global.sps_lim);
	info[INF_MAX_SESS_RATE]                  = mkf_u32(FN_RATE, global.sps_max);

#ifdef USE_OPENSSL
	info[INF_SSL_RATE]                       = (flags & STAT_USE_FLOAT) ? mkf_flt(FN_RATE, ssl_sess_rate) : mkf_u32(FN_RATE, ssl_sess_rate);
	info[INF_SSL_RATE_LIMIT]                 = mkf_u32(FO_CONFIG|FN_LIMIT, global.ssl_lim);
	info[INF_MAX_SSL_RATE]                   = mkf_u32(FN_MAX, global.ssl_max);
	info[INF_SSL_FRONTEND_KEY_RATE]          = (flags & STAT_USE_FLOAT) ? mkf_flt(FN_RATE, ssl_key_rate) : mkf_u32(0, ssl_key_rate);
	info[INF_SSL_FRONTEND_MAX_KEY_RATE]      = mkf_u32(FN_MAX, global.ssl_fe_keys_max);
	info[INF_SSL_FRONTEND_SESSION_REUSE_PCT] = (flags & STAT_USE_FLOAT) ? mkf_flt(FN_RATE, ssl_reuse) : mkf_u32(0, ssl_reuse);
	info[INF_SSL_BACKEND_KEY_RATE]           = (flags & STAT_USE_FLOAT) ? mkf_flt(FN_RATE, read_freq_ctr_flt(&global.ssl_be_keys_per_sec)) : mkf_u32(FN_RATE, read_freq_ctr(&global.ssl_be_keys_per_sec));
	info[INF_SSL_BACKEND_MAX_KEY_RATE]       = mkf_u32(FN_MAX, global.ssl_be_keys_max);
	info[INF_SSL_CACHE_LOOKUPS]              = mkf_u32(FN_COUNTER, global.shctx_lookups);
	info[INF_SSL_CACHE_MISSES]               = mkf_u32(FN_COUNTER, global.shctx_misses);
#endif
	info[INF_COMPRESS_BPS_IN]                = (flags & STAT_USE_FLOAT) ? mkf_flt(FN_RATE, read_freq_ctr_flt(&global.comp_bps_in)) : mkf_u32(FN_RATE, read_freq_ctr(&global.comp_bps_in));
	info[INF_COMPRESS_BPS_OUT]               = (flags & STAT_USE_FLOAT) ? mkf_flt(FN_RATE, read_freq_ctr_flt(&global.comp_bps_out)) : mkf_u32(FN_RATE, read_freq_ctr(&global.comp_bps_out));
	info[INF_COMPRESS_BPS_RATE_LIM]          = mkf_u32(FO_CONFIG|FN_LIMIT, global.comp_rate_lim);
#ifdef USE_ZLIB
	info[INF_ZLIB_MEM_USAGE]                 = mkf_u32(0, zlib_used_memory);
	info[INF_MAX_ZLIB_MEM_USAGE]             = mkf_u32(FO_CONFIG|FN_LIMIT, global.maxzlibmem);
#endif
	info[INF_TASKS]                          = mkf_u32(0, total_allocated_tasks());
	info[INF_RUN_QUEUE]                      = mkf_u32(0, total_run_queues());
	info[INF_IDLE_PCT]                       = mkf_u32(FN_AVG, clock_report_idle());
	info[INF_NODE]                           = mkf_str(FO_CONFIG|FN_OUTPUT|FS_SERVICE, global.node);
	if (global.desc)
		info[INF_DESCRIPTION]            = mkf_str(FO_CONFIG|FN_OUTPUT|FS_SERVICE, global.desc);
	info[INF_STOPPING]                       = mkf_u32(0, stopping);
	info[INF_JOBS]                           = mkf_u32(0, jobs);
	info[INF_UNSTOPPABLE_JOBS]               = mkf_u32(0, unstoppable_jobs);
	info[INF_LISTENERS]                      = mkf_u32(0, listeners);
	info[INF_ACTIVE_PEERS]                   = mkf_u32(0, active_peers);
	info[INF_CONNECTED_PEERS]                = mkf_u32(0, connected_peers);
	info[INF_DROPPED_LOGS]                   = mkf_u32(0, dropped_logs);
	info[INF_BUSY_POLLING]                   = mkf_u32(0, !!(global.tune.options & GTUNE_BUSY_POLLING));
	info[INF_FAILED_RESOLUTIONS]             = mkf_u32(0, resolv_failed_resolutions);
	info[INF_TOTAL_BYTES_OUT]                = mkf_u64(0, glob_out_bytes);
	info[INF_TOTAL_SPLICED_BYTES_OUT]        = mkf_u64(0, glob_spl_bytes);
	info[INF_BYTES_OUT_RATE]                 = mkf_u64(FN_RATE, glob_out_b32);
	info[INF_DEBUG_COMMANDS_ISSUED]          = mkf_u32(0, debug_commands_issued);
	info[INF_CUM_LOG_MSGS]                   = mkf_u32(FN_COUNTER, cum_log_messages);

	info[INF_TAINTED]                        = mkf_str(FO_STATUS, chunk_newstr(out));
	chunk_appendf(out, "%#x", get_tainted());
	info[INF_WARNINGS]                       = mkf_u32(FN_COUNTER, HA_ATOMIC_LOAD(&tot_warnings));
	info[INF_MAXCONN_REACHED]                = mkf_u32(FN_COUNTER, HA_ATOMIC_LOAD(&maxconn_reached));
	info[INF_BOOTTIME_MS]                    = mkf_u32(FN_DURATION, boot);

	return 1;
}

/* This function dumps information onto the stream connector's read buffer.
 * It returns 0 as long as it does not complete, non-zero upon completion.
 * No state is used.
 */
static int stats_dump_info_to_buffer(struct stconn *sc)
{
	struct appctx *appctx = __sc_appctx(sc);
	struct show_stat_ctx *ctx = appctx->svcctx;
	int ret;
	int current_field;

	if (!stats_fill_info(info, INF_TOTAL_FIELDS, ctx->flags))
		return 0;

	chunk_reset(&trash_chunk);
more:
	current_field = ctx->field;

	if (ctx->flags & STAT_FMT_TYPED)
		ret = stats_dump_typed_info_fields(&trash_chunk, info, ctx);
	else if (ctx->flags & STAT_FMT_JSON)
		ret = stats_dump_json_info_fields(&trash_chunk, info, ctx);
	else
		ret = stats_dump_info_fields(&trash_chunk, info, ctx);

	if (applet_putchk(appctx, &trash_chunk) == -1) {
		/* restore previous field */
		ctx->field = current_field;
		return 0;
	}
	if (ret && ctx->field) {
		/* partial dump */
		goto more;
	}
	ctx->field = 0;
	return 1;
}

/* This function dumps the schema onto the stream connector's read buffer.
 * It returns 0 as long as it does not complete, non-zero upon completion.
 * No state is used.
 *
 * Integer values bounded to the range [-(2**53)+1, (2**53)-1] as
 * per the recommendation for interoperable integers in section 6 of RFC 7159.
 */
static void stats_dump_json_schema(struct buffer *out)
{

	int old_len = out->data;

	chunk_strcat(out,
		     "{"
		      "\"$schema\":\"http://json-schema.org/draft-04/schema#\","
		      "\"oneOf\":["
		       "{"
			"\"title\":\"Info\","
			"\"type\":\"array\","
			"\"items\":{"
			 "\"title\":\"InfoItem\","
			 "\"type\":\"object\","
			 "\"properties\":{"
			  "\"field\":{\"$ref\":\"#/definitions/field\"},"
			  "\"processNum\":{\"$ref\":\"#/definitions/processNum\"},"
			  "\"tags\":{\"$ref\":\"#/definitions/tags\"},"
			  "\"value\":{\"$ref\":\"#/definitions/typedValue\"}"
			 "},"
			 "\"required\":[\"field\",\"processNum\",\"tags\","
				       "\"value\"]"
			"}"
		       "},"
		       "{"
			"\"title\":\"Stat\","
			"\"type\":\"array\","
			"\"items\":{"
			 "\"title\":\"InfoItem\","
			 "\"type\":\"object\","
			 "\"properties\":{"
			  "\"objType\":{"
			   "\"enum\":[\"Frontend\",\"Backend\",\"Listener\","
				     "\"Server\",\"Unknown\"]"
			  "},"
			  "\"proxyId\":{"
			   "\"type\":\"integer\","
			   "\"minimum\":0"
			  "},"
			  "\"id\":{"
			   "\"type\":\"integer\","
			   "\"minimum\":0"
			  "},"
			  "\"field\":{\"$ref\":\"#/definitions/field\"},"
			  "\"processNum\":{\"$ref\":\"#/definitions/processNum\"},"
			  "\"tags\":{\"$ref\":\"#/definitions/tags\"},"
			  "\"typedValue\":{\"$ref\":\"#/definitions/typedValue\"}"
			 "},"
			 "\"required\":[\"objType\",\"proxyId\",\"id\","
				       "\"field\",\"processNum\",\"tags\","
				       "\"value\"]"
			"}"
		       "},"
		       "{"
			"\"title\":\"Error\","
			"\"type\":\"object\","
			"\"properties\":{"
			 "\"errorStr\":{"
			  "\"type\":\"string\""
			 "}"
			"},"
			"\"required\":[\"errorStr\"]"
		       "}"
		      "],"
		      "\"definitions\":{"
		       "\"field\":{"
			"\"type\":\"object\","
			"\"pos\":{"
			 "\"type\":\"integer\","
			 "\"minimum\":0"
			"},"
			"\"name\":{"
			 "\"type\":\"string\""
			"},"
			"\"required\":[\"pos\",\"name\"]"
		       "},"
		       "\"processNum\":{"
			"\"type\":\"integer\","
			"\"minimum\":1"
		       "},"
		       "\"tags\":{"
			"\"type\":\"object\","
			"\"origin\":{"
			 "\"type\":\"string\","
			 "\"enum\":[\"Metric\",\"Status\",\"Key\","
				   "\"Config\",\"Product\",\"Unknown\"]"
			"},"
			"\"nature\":{"
			 "\"type\":\"string\","
			 "\"enum\":[\"Gauge\",\"Limit\",\"Min\",\"Max\","
				   "\"Rate\",\"Counter\",\"Duration\","
				   "\"Age\",\"Time\",\"Name\",\"Output\","
				   "\"Avg\", \"Unknown\"]"
			"},"
			"\"scope\":{"
			 "\"type\":\"string\","
			 "\"enum\":[\"Cluster\",\"Process\",\"Service\","
				   "\"System\",\"Unknown\"]"
			"},"
			"\"required\":[\"origin\",\"nature\",\"scope\"]"
		       "},"
		       "\"typedValue\":{"
			"\"type\":\"object\","
			"\"oneOf\":["
			 "{\"$ref\":\"#/definitions/typedValue/definitions/s32Value\"},"
			 "{\"$ref\":\"#/definitions/typedValue/definitions/s64Value\"},"
			 "{\"$ref\":\"#/definitions/typedValue/definitions/u32Value\"},"
			 "{\"$ref\":\"#/definitions/typedValue/definitions/u64Value\"},"
			 "{\"$ref\":\"#/definitions/typedValue/definitions/strValue\"}"
			"],"
			"\"definitions\":{"
			 "\"s32Value\":{"
			  "\"properties\":{"
			   "\"type\":{"
			    "\"type\":\"string\","
			    "\"enum\":[\"s32\"]"
			   "},"
			   "\"value\":{"
			    "\"type\":\"integer\","
			    "\"minimum\":-2147483648,"
			    "\"maximum\":2147483647"
			   "}"
			  "},"
			  "\"required\":[\"type\",\"value\"]"
			 "},"
			 "\"s64Value\":{"
			  "\"properties\":{"
			   "\"type\":{"
			    "\"type\":\"string\","
			    "\"enum\":[\"s64\"]"
			   "},"
			   "\"value\":{"
			    "\"type\":\"integer\","
			    "\"minimum\":-9007199254740991,"
			    "\"maximum\":9007199254740991"
			   "}"
			  "},"
			  "\"required\":[\"type\",\"value\"]"
			 "},"
			 "\"u32Value\":{"
			  "\"properties\":{"
			   "\"type\":{"
			    "\"type\":\"string\","
			    "\"enum\":[\"u32\"]"
			   "},"
			   "\"value\":{"
			    "\"type\":\"integer\","
			    "\"minimum\":0,"
			    "\"maximum\":4294967295"
			   "}"
			  "},"
			  "\"required\":[\"type\",\"value\"]"
			 "},"
			 "\"u64Value\":{"
			  "\"properties\":{"
			   "\"type\":{"
			    "\"type\":\"string\","
			    "\"enum\":[\"u64\"]"
			   "},"
			   "\"value\":{"
			    "\"type\":\"integer\","
			    "\"minimum\":0,"
			    "\"maximum\":9007199254740991"
			   "}"
			  "},"
			  "\"required\":[\"type\",\"value\"]"
			 "},"
			 "\"strValue\":{"
			  "\"properties\":{"
			   "\"type\":{"
			    "\"type\":\"string\","
			    "\"enum\":[\"str\"]"
			   "},"
			   "\"value\":{\"type\":\"string\"}"
			  "},"
			  "\"required\":[\"type\",\"value\"]"
			 "},"
			 "\"unknownValue\":{"
			  "\"properties\":{"
			   "\"type\":{"
			    "\"type\":\"integer\","
			    "\"minimum\":0"
			   "},"
			   "\"value\":{"
			    "\"type\":\"string\","
			    "\"enum\":[\"unknown\"]"
			   "}"
			  "},"
			  "\"required\":[\"type\",\"value\"]"
			 "}"
			"}"
		       "}"
		      "}"
		     "}");

	if (old_len == out->data) {
		chunk_reset(out);
		chunk_appendf(out,
			      "{\"errorStr\":\"output buffer too short\"}");
	}
	chunk_appendf(out, "\n");
}

/* This function dumps the schema onto the stream connector's read buffer.
 * It returns 0 as long as it does not complete, non-zero upon completion.
 * No state is used.
 */
static int stats_dump_json_schema_to_buffer(struct appctx *appctx)
{

	chunk_reset(&trash_chunk);

	stats_dump_json_schema(&trash_chunk);

	if (applet_putchk(appctx, &trash_chunk) == -1)
		return 0;

	return 1;
}

static int cli_parse_clear_counters(char **args, char *payload, struct appctx *appctx, void *private)
{
	struct proxy *px;
	struct server *sv;
	struct listener *li;
	struct stats_module *mod;
	int clrall = 0;

	if (strcmp(args[2], "all") == 0)
		clrall = 1;

	/* check permissions */
	if (!cli_has_level(appctx, ACCESS_LVL_OPER) ||
	    (clrall && !cli_has_level(appctx, ACCESS_LVL_ADMIN)))
		return 1;

	for (px = proxies_list; px; px = px->next) {
		if (clrall) {
			memset(&px->be_counters, 0, sizeof(px->be_counters));
			memset(&px->fe_counters, 0, sizeof(px->fe_counters));
		}
		else {
			px->be_counters.conn_max = 0;
			px->be_counters.p.http.rps_max = 0;
			px->be_counters.sps_max = 0;
			px->be_counters.cps_max = 0;
			px->be_counters.nbpend_max = 0;
			px->be_counters.qtime_max = 0;
			px->be_counters.ctime_max = 0;
			px->be_counters.dtime_max = 0;
			px->be_counters.ttime_max = 0;

			px->fe_counters.conn_max = 0;
			px->fe_counters.p.http.rps_max = 0;
			px->fe_counters.sps_max = 0;
			px->fe_counters.cps_max = 0;
		}

		for (sv = px->srv; sv; sv = sv->next)
			if (clrall)
				memset(&sv->counters, 0, sizeof(sv->counters));
			else {
				sv->counters.cur_sess_max = 0;
				sv->counters.nbpend_max = 0;
				sv->counters.sps_max = 0;
				sv->counters.qtime_max = 0;
				sv->counters.ctime_max = 0;
				sv->counters.dtime_max = 0;
				sv->counters.ttime_max = 0;
			}

		list_for_each_entry(li, &px->conf.listeners, by_fe)
			if (li->counters) {
				if (clrall)
					memset(li->counters, 0, sizeof(*li->counters));
				else
					li->counters->conn_max = 0;
			}
	}

	global.cps_max = 0;
	global.sps_max = 0;
	global.ssl_max = 0;
	global.ssl_fe_keys_max = 0;
	global.ssl_be_keys_max = 0;

	list_for_each_entry(mod, &stats_module_list[STATS_DOMAIN_PROXY], list) {
		if (!mod->clearable && !clrall)
			continue;

		for (px = proxies_list; px; px = px->next) {
			enum stats_domain_px_cap mod_cap = stats_px_get_cap(mod->domain_flags);

			if (px->cap & PR_CAP_FE && mod_cap & STATS_PX_CAP_FE) {
				EXTRA_COUNTERS_INIT(px->extra_counters_fe,
				                    mod,
				                    mod->counters,
				                    mod->counters_size);
			}

			if (px->cap & PR_CAP_BE && mod_cap & STATS_PX_CAP_BE) {
				EXTRA_COUNTERS_INIT(px->extra_counters_be,
				                    mod,
				                    mod->counters,
				                    mod->counters_size);
			}

			if (mod_cap & STATS_PX_CAP_SRV) {
				for (sv = px->srv; sv; sv = sv->next) {
					EXTRA_COUNTERS_INIT(sv->extra_counters,
				                            mod,
					                    mod->counters,
					                    mod->counters_size);
				}
			}

			if (mod_cap & STATS_PX_CAP_LI) {
				list_for_each_entry(li, &px->conf.listeners, by_fe) {
					EXTRA_COUNTERS_INIT(li->extra_counters,
				                            mod,
					                    mod->counters,
					                    mod->counters_size);
				}
			}
		}
	}

	resolv_stats_clear_counters(clrall, &stats_module_list[STATS_DOMAIN_RESOLVERS]);

	memset(activity, 0, sizeof(activity));
	return 1;
}


static int cli_parse_show_info(char **args, char *payload, struct appctx *appctx, void *private)
{
	struct show_stat_ctx *ctx = applet_reserve_svcctx(appctx, sizeof(*ctx));
	int arg = 2;

	ctx->scope_str = 0;
	ctx->scope_len = 0;
	ctx->flags = 0;
	ctx->field = 0; /* explicit default value */

	while (*args[arg]) {
		if (strcmp(args[arg], "typed") == 0)
			ctx->flags = (ctx->flags & ~STAT_FMT_MASK) | STAT_FMT_TYPED;
		else if (strcmp(args[arg], "json") == 0)
			ctx->flags = (ctx->flags & ~STAT_FMT_MASK) | STAT_FMT_JSON;
		else if (strcmp(args[arg], "desc") == 0)
			ctx->flags |= STAT_SHOW_FDESC;
		else if (strcmp(args[arg], "float") == 0)
			ctx->flags |= STAT_USE_FLOAT;
		arg++;
	}
	return 0;
}


static int cli_parse_show_stat(char **args, char *payload, struct appctx *appctx, void *private)
{
	struct show_stat_ctx *ctx = applet_reserve_svcctx(appctx, sizeof(*ctx));
	int arg = 2;

	ctx->scope_str = 0;
	ctx->scope_len = 0;
	ctx->flags = STAT_SHNODE | STAT_SHDESC;

	if ((strm_li(appctx_strm(appctx))->bind_conf->level & ACCESS_LVL_MASK) >= ACCESS_LVL_OPER)
		ctx->flags |= STAT_SHLGNDS;

	/* proxy is the default domain */
	ctx->domain = STATS_DOMAIN_PROXY;
	if (strcmp(args[arg], "domain") == 0) {
		++args;

		if (strcmp(args[arg], "proxy") == 0) {
			++args;
		} else if (strcmp(args[arg], "resolvers") == 0) {
			ctx->domain = STATS_DOMAIN_RESOLVERS;
			++args;
		} else {
			return cli_err(appctx, "Invalid statistics domain.\n");
		}
	}

	if (ctx->domain == STATS_DOMAIN_PROXY
	    && *args[arg] && *args[arg+1] && *args[arg+2]) {
		struct proxy *px;

		px = proxy_find_by_name(args[arg], 0, 0);
		if (px)
			ctx->iid = px->uuid;
		else
			ctx->iid = atoi(args[arg]);

		if (!ctx->iid)
			return cli_err(appctx, "No such proxy.\n");

		ctx->flags |= STAT_BOUND;
		ctx->type = atoi(args[arg+1]);
		ctx->sid = atoi(args[arg+2]);
		arg += 3;
	}

	while (*args[arg]) {
		if (strcmp(args[arg], "typed") == 0)
			ctx->flags = (ctx->flags & ~STAT_FMT_MASK) | STAT_FMT_TYPED;
		else if (strcmp(args[arg], "json") == 0)
			ctx->flags = (ctx->flags & ~STAT_FMT_MASK) | STAT_FMT_JSON;
		else if (strcmp(args[arg], "desc") == 0)
			ctx->flags |= STAT_SHOW_FDESC;
		else if (strcmp(args[arg], "no-maint") == 0)
			ctx->flags |= STAT_HIDE_MAINT;
		else if (strcmp(args[arg], "up") == 0)
			ctx->flags |= STAT_HIDE_DOWN;
		arg++;
	}

	return 0;
}

static int cli_io_handler_dump_info(struct appctx *appctx)
{
	trash_chunk = b_make(trash.area, trash.size, 0, 0);
	return stats_dump_info_to_buffer(appctx_sc(appctx));
}

/* This I/O handler runs as an applet embedded in a stream connector. It is
 * used to send raw stats over a socket.
 */
static int cli_io_handler_dump_stat(struct appctx *appctx)
{
	trash_chunk = b_make(trash.area, trash.size, 0, 0);
	return stats_dump_stat_to_buffer(appctx_sc(appctx), NULL, NULL);
}

static int cli_io_handler_dump_json_schema(struct appctx *appctx)
{
	trash_chunk = b_make(trash.area, trash.size, 0, 0);
	return stats_dump_json_schema_to_buffer(appctx);
}

int stats_allocate_proxy_counters_internal(struct extra_counters **counters,
                                           int type, int px_cap)
{
	struct stats_module *mod;

	EXTRA_COUNTERS_REGISTER(counters, type, alloc_failed);

	list_for_each_entry(mod, &stats_module_list[STATS_DOMAIN_PROXY], list) {
		if (!(stats_px_get_cap(mod->domain_flags) & px_cap))
			continue;

		EXTRA_COUNTERS_ADD(mod, *counters, mod->counters, mod->counters_size);
	}

	EXTRA_COUNTERS_ALLOC(*counters, alloc_failed);

	list_for_each_entry(mod, &stats_module_list[STATS_DOMAIN_PROXY], list) {
		if (!(stats_px_get_cap(mod->domain_flags) & px_cap))
			continue;

		EXTRA_COUNTERS_INIT(*counters, mod, mod->counters, mod->counters_size);
	}

	return 1;

  alloc_failed:
	return 0;
}

/* Initialize and allocate all extra counters for a proxy and its attached
 * servers/listeners with all already registered stats module
 */
int stats_allocate_proxy_counters(struct proxy *px)
{
	struct server *sv;
	struct listener *li;

	if (px->cap & PR_CAP_FE) {
		if (!stats_allocate_proxy_counters_internal(&px->extra_counters_fe,
		                                            COUNTERS_FE,
		                                            STATS_PX_CAP_FE)) {
			return 0;
		}
	}

	if (px->cap & PR_CAP_BE) {
		if (!stats_allocate_proxy_counters_internal(&px->extra_counters_be,
		                                            COUNTERS_BE,
		                                            STATS_PX_CAP_BE)) {
			return 0;
		}
	}

	for (sv = px->srv; sv; sv = sv->next) {
		if (!stats_allocate_proxy_counters_internal(&sv->extra_counters,
		                                            COUNTERS_SV,
		                                            STATS_PX_CAP_SRV)) {
			return 0;
		}
	}

	list_for_each_entry(li, &px->conf.listeners, by_fe) {
		if (!stats_allocate_proxy_counters_internal(&li->extra_counters,
		                                            COUNTERS_LI,
		                                            STATS_PX_CAP_LI)) {
			return 0;
		}
	}

	return 1;
}

void stats_register_module(struct stats_module *m)
{
	const uint8_t domain = stats_get_domain(m->domain_flags);

	LIST_APPEND(&stats_module_list[domain], &m->list);
	stat_count[domain] += m->stats_count;
}

static int allocate_stats_px_postcheck(void)
{
	struct stats_module *mod;
	size_t i = ST_F_TOTAL_FIELDS;
	int err_code = 0;
	struct proxy *px;

	stat_count[STATS_DOMAIN_PROXY] += ST_F_TOTAL_FIELDS;

	stat_f[STATS_DOMAIN_PROXY] = malloc(stat_count[STATS_DOMAIN_PROXY] * sizeof(struct name_desc));
	if (!stat_f[STATS_DOMAIN_PROXY]) {
		ha_alert("stats: cannot allocate all fields for proxy statistics\n");
		err_code |= ERR_ALERT | ERR_FATAL;
		return err_code;
	}

	memcpy(stat_f[STATS_DOMAIN_PROXY], stat_fields,
	       ST_F_TOTAL_FIELDS * sizeof(struct name_desc));

	list_for_each_entry(mod, &stats_module_list[STATS_DOMAIN_PROXY], list) {
		memcpy(stat_f[STATS_DOMAIN_PROXY] + i,
		       mod->stats,
		       mod->stats_count * sizeof(struct name_desc));
		i += mod->stats_count;
	}

	for (px = proxies_list; px; px = px->next) {
		if (!stats_allocate_proxy_counters(px)) {
			ha_alert("stats: cannot allocate all counters for proxy statistics\n");
			err_code |= ERR_ALERT | ERR_FATAL;
			return err_code;
		}
	}

	/* wait per-thread alloc to perform corresponding stat_l allocation */

	return err_code;
}

REGISTER_CONFIG_POSTPARSER("allocate-stats-px", allocate_stats_px_postcheck);

static int allocate_stats_rslv_postcheck(void)
{
	struct stats_module *mod;
	size_t i = 0;
	int err_code = 0;

	stat_f[STATS_DOMAIN_RESOLVERS] = malloc(stat_count[STATS_DOMAIN_RESOLVERS] * sizeof(struct name_desc));
	if (!stat_f[STATS_DOMAIN_RESOLVERS]) {
		ha_alert("stats: cannot allocate all fields for resolver statistics\n");
		err_code |= ERR_ALERT | ERR_FATAL;
		return err_code;
	}

	list_for_each_entry(mod, &stats_module_list[STATS_DOMAIN_RESOLVERS], list) {
		memcpy(stat_f[STATS_DOMAIN_RESOLVERS] + i,
		       mod->stats,
		       mod->stats_count * sizeof(struct name_desc));
		i += mod->stats_count;
	}

	if (!resolv_allocate_counters(&stats_module_list[STATS_DOMAIN_RESOLVERS])) {
		ha_alert("stats: cannot allocate all counters for resolver statistics\n");
		err_code |= ERR_ALERT | ERR_FATAL;
		return err_code;
	}

	/* wait per-thread alloc to perform corresponding stat_l allocation */

	return err_code;
}

REGISTER_CONFIG_POSTPARSER("allocate-stats-resolver", allocate_stats_rslv_postcheck);

static int allocate_stat_lines_per_thread(void)
{
	int domains[] = { STATS_DOMAIN_PROXY, STATS_DOMAIN_RESOLVERS }, i;

	for (i = 0; i < STATS_DOMAIN_COUNT; ++i) {
		const int domain = domains[i];

		stat_l[domain] = malloc(stat_count[domain] * sizeof(struct field));
		if (!stat_l[domain])
			return 0;
	}
	return 1;
}

REGISTER_PER_THREAD_ALLOC(allocate_stat_lines_per_thread);

static int allocate_trash_counters(void)
{
	struct stats_module *mod;
	int domains[] = { STATS_DOMAIN_PROXY, STATS_DOMAIN_RESOLVERS }, i;
	size_t max_counters_size = 0;

	/* calculate the greatest counters used by any stats modules */
	for (i = 0; i < STATS_DOMAIN_COUNT; ++i) {
		list_for_each_entry(mod, &stats_module_list[domains[i]], list) {
			max_counters_size = mod->counters_size > max_counters_size ?
			                    mod->counters_size : max_counters_size;
		}
	}

	/* allocate the trash with the size of the greatest counters */
	if (max_counters_size) {
		trash_counters = malloc(max_counters_size);
		if (!trash_counters) {
			ha_alert("stats: cannot allocate trash counters for statistics\n");
			return 0;
		}
	}

	return 1;
}

REGISTER_PER_THREAD_ALLOC(allocate_trash_counters);

static void deinit_stat_lines_per_thread(void)
{
	int domains[] = { STATS_DOMAIN_PROXY, STATS_DOMAIN_RESOLVERS }, i;

	for (i = 0; i < STATS_DOMAIN_COUNT; ++i) {
		const int domain = domains[i];

		ha_free(&stat_l[domain]);
	}
}


REGISTER_PER_THREAD_FREE(deinit_stat_lines_per_thread);

static void deinit_stats(void)
{
	int domains[] = { STATS_DOMAIN_PROXY, STATS_DOMAIN_RESOLVERS }, i;

	for (i = 0; i < STATS_DOMAIN_COUNT; ++i) {
		const int domain = domains[i];

		if (stat_f[domain])
			free(stat_f[domain]);
	}
}

REGISTER_POST_DEINIT(deinit_stats);

static void free_trash_counters(void)
{
	if (trash_counters)
		free(trash_counters);
}

REGISTER_PER_THREAD_FREE(free_trash_counters);

/* register cli keywords */
static struct cli_kw_list cli_kws = {{ },{
	{ { "clear", "counters",  NULL },      "clear counters [all]                    : clear max statistics counters (or all counters)", cli_parse_clear_counters, NULL, NULL },
	{ { "show", "info",  NULL },           "show info [desc|json|typed|float]*      : report information about the running process",    cli_parse_show_info, cli_io_handler_dump_info, NULL },
	{ { "show", "stat",  NULL },           "show stat [desc|json|no-maint|typed|up]*: report counters for each proxy and server",       cli_parse_show_stat, cli_io_handler_dump_stat, NULL },
	{ { "show", "schema",  "json", NULL }, "show schema json                        : report schema used for stats",                    NULL, cli_io_handler_dump_json_schema, NULL },
	{{},}
}};

INITCALL1(STG_REGISTER, cli_register_kw, &cli_kws);

struct applet http_stats_applet = {
	.obj_type = OBJ_TYPE_APPLET,
	.name = "<STATS>", /* used for logging */
	.fct = http_stats_io_handler,
	.release = NULL,
};

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
