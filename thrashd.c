#include <pwd.h>
#include <grp.h>

#include <sys/resource.h>

#include "thrasher.h"
#include "version.h"
#include "httpd.h"
#include "filemac.h"

#ifdef WITH_GEOIP
#include <GeoIP.h>
GeoIP *gi = 0;
#endif

#define MAX_URI_SIZE  1500
#define MAX_HOST_SIZE 1500

/*
 * blah
 */

char           *process_name;
uint32_t        uri_check;
uint32_t        host_check;
uint32_t        addr_check;
char           *bind_addr;
uint16_t        bind_port;
uint32_t        soft_block_timeout;
uint32_t        hard_block_timeout;
block_ratio_t   host_ratio;
block_ratio_t   uri_ratio;
block_ratio_t   addr_ratio;
struct event    server_event;
int             server_port;
uint32_t        qps;
uint32_t        qps_last;
struct event    qps_event;
static int      rundaemon;
int             syslog_enabled;
char           *syslog_facility;
char           *rbl_zone;
char           *rbl_ns;
int             rbl_max_queries;
int             rbl_queries;
uint32_t        rbl_negcache_timeout;
uint64_t        total_blocked_connections;
uint64_t        total_queries;
uint32_t        connection_timeout;
GSList         *current_connections;
int             current_connections_count;
GTree          *current_blocks;
GHashTable     *uri_table;
GHashTable     *host_table;
GHashTable     *addr_table;
GTree          *rbl_negative_cache;
GHashTable     *uri_states;
GHashTable     *host_states;
GTree          *recently_blocked;
GHashTable     *uris_ratio_table;
GRand          *randdata;
FILE           *logfile;
char           *http_password;
char           *backup_file;
struct event    backup_event;
char           *config_filename;
uint32_t        debug;
char           *geoip_file;
char           *ip_action_url;

uint32_t recently_blocked_timeout;
block_ratio_t minimum_random_ratio;
block_ratio_t maximum_random_ratio;

char           *drop_user;
char           *drop_group;

int             velocity_num;

#ifdef WITH_BGP
char           *bgp_sockname;
int             bgp_sock;
#endif

void
reset_query(query_t * query)
{
    if (query->uri)
        free(query->uri);
    if (query->host)
        free(query->host);

    query->uri = NULL;
    query->host = NULL;
    query->uri_len = 0;
    query->host_len = 0;
}

void
free_client_conn(client_conn_t * conn)
{
    if (!conn)
        return;

    if (debug)
        LOG(logfile, "Lost connection from %s:%d",
            inet_ntoa(*(struct in_addr *) &conn->conn_addr),
            ntohs(conn->conn_port));

    if (connection_timeout) /* Stop the warning */
        evtimer_del(&conn->timeout);
    event_del(&conn->event);

    reset_iov(&conn->data);
    reset_query(&conn->query);

    current_connections_count--;
    current_connections =
        g_slist_remove(current_connections, (gconstpointer) conn);

    close(conn->sock);
    free(conn);
}


int
uint32_cmp(const void *a, const void *b)
{
    if (*(uint32_t *) a < *(uint32_t *) b)
        return -1;

    if (*(uint32_t *) a > *(uint32_t *) b)
        return 1;

    return 0;
}

void increase_limits()
{

    struct rlimit l;

    getrlimit(RLIMIT_NOFILE, &l);
    l.rlim_cur = l.rlim_max;
    setrlimit(RLIMIT_NOFILE, &l);
}

void
globals_init(void)
{
    connection_timeout = 0;
    uri_check = 1;
    host_check = 1;
    addr_check = 1;
    bind_addr = "0.0.0.0";
    bind_port = 1972;
    server_port = 1979;
    soft_block_timeout = 60;
    hard_block_timeout = 0;
    host_ratio.num_connections = 10;
    uri_ratio.num_connections = 10;
    host_ratio.timelimit = 60;
    uri_ratio.timelimit = 60;
    minimum_random_ratio.num_connections = 0;
    minimum_random_ratio.timelimit = 0;
    maximum_random_ratio.timelimit = 0;
    maximum_random_ratio.num_connections = 0;
    addr_ratio.num_connections = 100;
    addr_ratio.timelimit = 10;
    qps = 0;
    qps_last = 0;
    current_connections_count = 0;
    current_connections = g_slist_alloc();
    current_blocks = g_tree_new((GCompareFunc) uint32_cmp);
    uri_table = g_hash_table_new(g_str_hash, g_str_equal);
    host_table = g_hash_table_new(g_str_hash, g_str_equal);
    addr_table = g_hash_table_new(g_str_hash, g_str_equal);
    process_name = g_strdup("default");
    syslog_enabled = 1;
    rundaemon = 0;
    rbl_zone = NULL;
    rbl_negative_cache = g_tree_new((GCompareFunc) uint32_cmp);
    rbl_negcache_timeout = 10;
    rbl_max_queries = 0;
    rbl_queries = 0;
    total_blocked_connections = 0;
    total_queries = 0;
    randdata = NULL;
    recently_blocked = NULL;
    recently_blocked_timeout = 0;
    drop_user = NULL;
    drop_group = NULL;
    uris_ratio_table = NULL;
    logfile = stdout;
    syslog_facility = g_strdup("local6");
    velocity_num = 100;
    geoip_file = 0;
    ip_action_url = 0;
#if DEBUG
    debug = 1;
#else
    debug = 0;
#endif
}

int
set_nb(int sock)
{
    return fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
}

void
timeout_client(int sock, short which, client_conn_t * conn)
{
    LOG(logfile, "Connection idle for %d seconds, disconnecting client",
        connection_timeout);
    free_client_conn(conn);
}

void
reset_connection_timeout(client_conn_t * conn, uint32_t timeout)
{
    struct timeval  tv;

    if (!timeout)
        return;

    tv.tv_sec = timeout;
    tv.tv_usec = 0;

	
#ifdef DEBUG
    LOG(logfile, "%p", conn);
#endif

    evtimer_del(&conn->timeout);
    evtimer_set(&conn->timeout, (void *) timeout_client, conn);
    evtimer_add(&conn->timeout, &tv);
}

void
slide_ratios(blocked_node_t * bnode)
{
    uint32_t        last_conn,
                    last_time;

    if (!bnode)
        return;

    last_conn = bnode->ratio.num_connections;
    last_time = bnode->ratio.timelimit;

    if (!last_conn && !last_time) {
        last_conn = g_rand_int_range(randdata,
                                     minimum_random_ratio.num_connections,
                                     maximum_random_ratio.num_connections);
        last_time = g_rand_int_range(randdata,
                                     minimum_random_ratio.timelimit,
                                     maximum_random_ratio.timelimit);
    } else {
        if (minimum_random_ratio.num_connections == last_conn)
            last_conn--;

        if (maximum_random_ratio.timelimit == last_time)
            last_time++;

        last_conn = g_rand_int_range(randdata,
                                     minimum_random_ratio.num_connections,
                                     last_conn);

        last_time = g_rand_int_range(randdata,
                                     last_time,
                                     maximum_random_ratio.timelimit);
    }

    if (last_conn <= minimum_random_ratio.num_connections ||
        last_time >= maximum_random_ratio.timelimit) {
        last_conn = g_rand_int_range(randdata,
                                     minimum_random_ratio.num_connections,
                                     maximum_random_ratio.num_connections);

        last_time = g_rand_int_range(randdata,
                                     minimum_random_ratio.timelimit,
                                     maximum_random_ratio.timelimit);
    }
    if (debug)
        LOG(logfile, "our new random ratio is %d:%d", last_conn, last_time);
    bnode->ratio.num_connections = last_conn;
    bnode->ratio.timelimit = last_time;
}

void
remove_holddown(uint32_t addr)
{
    g_tree_remove(current_blocks, &addr);
}

void
expire_bnode(int sock, short which, blocked_node_t * bnode)
{
    /*
     * blocked node expiration
     */
    LOG(logfile, "expired address %s after %u hits",
        inet_ntoa(*(struct in_addr *) &bnode->saddr), bnode->count);

    if (bnode->hard_timeout.ev_timeout.tv_sec)
        evtimer_del(&bnode->hard_timeout);
    evtimer_del(&bnode->timeout);
    remove_holddown(bnode->saddr);

    /*
     * which will tell us whether this is a timer or not,
     * since manual removes will set which to 0, we skip
     * recently_blocked insert
     */
    if (recently_blocked && which) {
        /*
         * if we have our moving ratios enabled we
         * put the blocked node into recently_blocked
         */
        struct timeval  tv;

        evtimer_del(&bnode->recent_block_timeout);

        /*
         * slide our windows around
         */
        slide_ratios(bnode);

        /*
         * reset our count
         */
        bnode->count = 0;

        /*
         * load this guy up into our recently blocked list
         */
        g_tree_insert(recently_blocked, &bnode->saddr, bnode);

        /*
         * set our timeout to the global
         */
        tv.tv_sec = recently_blocked_timeout;
        tv.tv_usec = 0;

        evtimer_set(&bnode->recent_block_timeout,
                    (void *) expire_recent_bnode, bnode);
        evtimer_add(&bnode->recent_block_timeout, &tv);

        LOG(logfile,
            "Placing %s into recently blocked list with a ratio of %d:%d",
            inet_ntoa(*(struct in_addr *) &bnode->saddr),
            bnode->ratio.num_connections, bnode->ratio.timelimit);
        return;
    }

    free(bnode);
}


void
expire_hard_bnode(int sock, short which, blocked_node_t * bnode) {
    char            akey[20];
    qstats_t       *stats;


    if (debug)
        LOG(logfile, "HARD Timeout expired (%d, %d, %p) for %s after %u hits",
            sock, which, bnode,
            inet_ntoa(*(struct in_addr *)&bnode->saddr), bnode->count);

    snprintf(akey, sizeof(akey), "%u", bnode->saddr);
    stats = g_hash_table_lookup(addr_table, akey);
    if (stats) {
        expire_stats_node(0, 0, stats);
    }

    return expire_bnode(sock, which, bnode);
}

void
expire_recent_bnode(int sock, short which, blocked_node_t * bnode)
{

    if (debug)
        LOG(logfile, "RECENT Timeout expired (%d, %d, %p) for %s after %u hits",
            sock, which, bnode,
            inet_ntoa(*(struct in_addr *)&bnode->saddr), bnode->count);

    evtimer_del(&bnode->recent_block_timeout);
    g_tree_remove(recently_blocked, &bnode->saddr);
    free(bnode);
}

void
expire_stats_node(int sock, short which, qstats_t * stat_node)
{
    if (debug)
        LOG(logfile, "STATS Timeout expired (%d, %d, %p) for %s (%p) after %u hits",
            sock, which, stat_node,
            stat_node->key, stat_node, stat_node->connections);

    /*
     * remove the timers
     */
    evtimer_del(&stat_node->timeout);

    /*
     * remove this entry from the designated hash table
     */
    g_hash_table_remove(stat_node->table, stat_node->key);

    free(stat_node->key);
    free(stat_node);
}

blocked_node_t *
block_addr(client_conn_t * conn, uint32_t addr)
{
    blocked_node_t *bnode;
    struct timeval  tv;

    /*
     * create a new blocked node structure
     */
    if (!(bnode = malloc(sizeof(blocked_node_t)))) {
        LOG(logfile, "Out of memory: %s", strerror(errno));
        exit(1);
    }

    memset(bnode, 0, sizeof(blocked_node_t));

    if (conn && conn->last_time.tv_sec)
        bnode->last_time = conn->last_time;
    else
        evutil_gettimeofday(&bnode->last_time, NULL);

    bnode->saddr = addr;
    bnode->count = 1;

    if (conn)
        bnode->first_seen_addr = conn->conn_addr;
    else
        /*
         * sometimes we don't get a conn struct, this can be due to other
         * types of blocking - RBL for example
         */
        bnode->first_seen_addr = 0;

    /*
     * insert the blocked node into our tree of held down addresses
     */
    g_tree_insert(current_blocks, &bnode->saddr, bnode);

    /*
     * add our soft timeout for this node
     */
    tv.tv_sec = soft_block_timeout;
    tv.tv_usec = 0;

    evtimer_set(&bnode->timeout, (void *) expire_bnode, bnode);
    evtimer_add(&bnode->timeout, &tv);

/* add our hard timeout, if the option is enabled */
    if (hard_block_timeout > 0) {
        tv.tv_sec  = hard_block_timeout;
        tv.tv_usec = 0;

        evtimer_set(&bnode->hard_timeout, (void *)expire_hard_bnode, bnode);
        evtimer_add(&bnode->hard_timeout, &tv);
    }
#ifdef WITH_BGP
    /*
     * XXX TEST XXX
     */
    bgp_community_t cm;
    cm.asn = 667;
    cm.community = 30;
    thrash_bgp_inject(addr, &cm, bgp_sock);
#endif

    return bnode;
}

qstats_t       *
create_stats_node(uint32_t saddr, const char *key, GHashTable * tbl)
{
    qstats_t       *snode;

    if (!saddr || !key || !tbl)
        return NULL;

    if (!(snode = calloc(sizeof(qstats_t), 1))) {
        LOG(logfile, "OOM: %s", strerror(errno));
        exit(1);
    }

    snode->saddr = saddr;
    snode->key = strdup(key);
    snode->table = tbl;

    return snode;
}

int
update_thresholds(client_conn_t * conn, char *key, stat_type_t type, block_ratio_t *ratio)
{
    GHashTable     *table;
    qstats_t       *stats;
    struct timeval  tv;

    switch (type) {
    case stat_type_uri:
        table = uri_table;
        break;
    case stat_type_host:
        table = host_table;
        break;
    case stat_type_address:
        table = addr_table;
        break;
    default:
        return 0;
    }

    stats = g_hash_table_lookup(table, key);

    if (!stats) {
        /*
         * create a new statistics table for this type
         */
        if (!(stats = create_stats_node(conn->query.saddr, key, table)))
            return -1;

        /*
         * insert the new statistics table into its hash
         */
        g_hash_table_insert(table, stats->key, stats);

        /*
         * now set an expire timer for this qstat_t node
         */
        tv.tv_sec = ratio->timelimit;
        tv.tv_usec = 0;

        evtimer_set(&stats->timeout, (void *) expire_stats_node, stats);
        evtimer_add(&stats->timeout, &tv);
    }
    if (debug)
        LOG(logfile, "Updating stats node %s (%p) type %d",
            stats->key, stats, type);

    /*
     * increment our connection counter
     */
    stats->connections++;

    if (stats->connections >= ratio->num_connections) {
        /*
         * we seemed to have hit a threshold
         */
        blocked_node_t *bnode;
        char           *blockedaddr;
        char           *triggeraddr;

        bnode = block_addr(conn, stats->saddr);

        blockedaddr = strdup(inet_ntoa(*(struct in_addr *) &bnode->saddr));
        triggeraddr =
            strdup(inet_ntoa(*(struct in_addr *) &bnode->first_seen_addr));

        LOG(logfile, "type %d holding down address %s triggered by %s (%d >= %d) key %s",
            type, blockedaddr, triggeraddr, stats->connections, ratio->num_connections, key);

        free(blockedaddr);
        free(triggeraddr);

        expire_stats_node(0, 0, stats);

        return 1;
    }

    /*
     * not blocked
     */
    return 0;
}

int
is_whitelisted(client_conn_t * conn)
{
    return 0;
}

int
do_thresholding(client_conn_t * conn)
{
    uint32_t        hkeylen,
                    ukeylen;
    char           *hkey,
                   *ukey;
    int             blocked;
    struct timeval  tv;
    blocked_node_t *bnode;
    uint64_t        arrival_gap;

    qps++;
    total_queries++;

    blocked = 0;
    ukey = NULL;
    hkey = NULL;

    if (debug) {
        char connip[40], queryip[40];
        strcpy(connip, inet_ntoa(*(struct in_addr *) &conn->conn_addr));
        strcpy(queryip, inet_ntoa(*(struct in_addr *) &conn->query.saddr));
        LOG(logfile, "Thresholding %d from %s for %s host %.*s uri %.*s",
            conn->type,
            connip,
            queryip,
            conn->query.host_len, conn->query.host,
            conn->query.uri_len, conn->query.uri);
    }
          

    /*
     * check to see if this address is whitelisted
     */

    if (is_whitelisted(conn))
        return 0;

    /*
     * check if we already have a block somewhere
     */
    if ((bnode = g_tree_lookup(current_blocks, &conn->query.saddr))) {
        /*
         * this connection seems to be blocked, reset our block timers
         * and continue on
         */
        tv.tv_sec = soft_block_timeout;
        tv.tv_usec = 0;

        evtimer_del(&bnode->timeout);
        evtimer_set(&bnode->timeout, (void *) expire_bnode, bnode);
        evtimer_add(&bnode->timeout, &tv);

        /*
         * increment our stats counter
         */
        bnode->count++;
        total_blocked_connections++;

        /* Calculate distance */
        arrival_gap = (conn->last_time.tv_sec - bnode->last_time.tv_sec) * 1000000
                    + (conn->last_time.tv_usec - bnode->last_time.tv_usec);

        bnode->avg_distance_usec = (bnode->avg_distance_usec * (velocity_num - 1) + arrival_gap)/velocity_num;

        bnode->last_time = conn->last_time;
        return 1;
    }

    if (((recently_blocked) && (bnode =
                                g_tree_lookup(recently_blocked,
                                              &conn->query.saddr)))) {
        /*
         * this address has been recently expired from the current_blocks
         * and placed into the recently_blocked list.
         */
        if (bnode->count++ == 0) {
            /*
             * This is the first packet we have seen from this address
             * since it was put into the recently_blocked tree.
             */
            evtimer_del(&bnode->recent_block_timeout);


            /*
             * since we only end up in the recently_blocked list after
             * a node has been blocked, then expired via expire_bnode(),
             * expire_bnode() shifts around the ratios (bnode->ratio)
             * randomly. We use this data to set our packets/window
             */
            tv.tv_sec = bnode->ratio.timelimit;
            tv.tv_usec = 0;

            /*
             * if this timer is every reached, it means that we never
             * hit our ratio, thus we need to expire it from the
             * recently_blocked list
             */

            evtimer_set(&bnode->recent_block_timeout,
                        (void *) expire_recent_bnode, bnode);
            evtimer_add(&bnode->recent_block_timeout, &tv);
        }

        if (bnode->count >= bnode->ratio.num_connections) {
            /*
             * this connection deserves to be blocked
             */

            /*
             * remove from our recently blocked list
             */
            evtimer_del(&bnode->recent_block_timeout);
            g_tree_remove(recently_blocked, &conn->query.saddr);

            /*
             * insert into our block tree
             */
            g_tree_insert(current_blocks, &bnode->saddr, bnode);

            /*
             * set our timeout to the normal timelimit
             */
            tv.tv_sec = soft_block_timeout;
            tv.tv_usec = 0;

            evtimer_set(&bnode->timeout, (void *) expire_bnode, bnode);
            evtimer_add(&bnode->timeout, &tv);

            return 1;
        }

        return 0;
    }

    /*
     * we are currently not blocked
     */

    /*
     * next we query our RBL server if applicable. NOTE: this will not
     * block immediately, this is a post check. This is so that we don't
     * block on the operation
     */

    if (rbl_zone)
        make_rbl_query(conn->query.saddr);

    switch (conn->type) {

    case TYPE_THRESHOLD_v1:
    case TYPE_THRESHOLD_v3:
        ukeylen = conn->query.uri_len + 14;

        if (!(ukey = calloc(ukeylen, 1))) {
            LOG(logfile, "Out of memory: %s", strerror(errno));
            exit(1);
        }

        hkeylen = conn->query.host_len + 14;

        if (!(hkey = calloc(hkeylen, 1))) {
            LOG(logfile, "Out of memory: %s", strerror(errno));
            exit(1);
        }

        snprintf(ukey, ukeylen - 1, "%u:%s", conn->query.saddr,
                 conn->query.uri);

        snprintf(hkey, hkeylen - 1, "%u:%s", conn->query.saddr,
                 conn->query.host);


        if (uri_check) {
            block_ratio_t *request_uri_ratio = NULL;

            if (uris_ratio_table)
                request_uri_ratio = g_hash_table_lookup(uris_ratio_table, conn->query.uri);

            if (!request_uri_ratio)
                request_uri_ratio = &uri_ratio;

            if (update_thresholds(conn, ukey, stat_type_uri, request_uri_ratio) == 1)
                blocked = 1;
        }

        if (host_check
            && update_thresholds(conn, hkey, stat_type_host, &host_ratio) == 1)
            blocked = 1;

        break;
    case TYPE_THRESHOLD_v2:
        /*
         * with v2 we only care about the source-address
         */

        if (addr_check <= 0)
            break;

        hkeylen = 13;

        if (!(hkey = calloc(hkeylen, 1))) {
            LOG(logfile, "Out of memory: %s", strerror(errno));
            exit(1);
        }

        snprintf(hkey, hkeylen - 1, "%u", conn->query.saddr);

        if (update_thresholds(conn, hkey, stat_type_address, &addr_ratio) == 1)
            blocked = 1;

        break;
    default:
        blocked = 0;
        break;
    }

    if (ukey)
        free(ukey);
    if (hkey)
        free(hkey);

    return blocked;
}

void
client_process_data(int sock, short which, client_conn_t * conn)
{
    int             ioret;
    int             blocked;

    reset_connection_timeout(conn, connection_timeout);

    if (!conn->data.buf)
        /*
         * if the connection has an ID, then set it to 5 bytes of reading,
         * else make it only 1 byte
         */
        initialize_iov(&conn->data,
                       conn->type == TYPE_THRESHOLD_v3 ?
                       sizeof(uint32_t) +
                       sizeof(uint8_t) : sizeof(uint8_t));

    if (do_thresholding(conn) == 1)
        blocked = 1;
    else
        blocked = 0;

#if DEBUG
    LOG(logfile, "saddr %u block stats: %d\n", conn->query.saddr, blocked);
#endif

    if (conn->type == TYPE_THRESHOLD_v3) {
        memcpy(conn->data.buf, &conn->id, sizeof(uint32_t));
        conn->data.buf[4] = blocked;
    } else
        *conn->data.buf = blocked;

    ioret = write_iov(&conn->data, sock);

    if (ioret < 0) {
        free_client_conn(conn);
        return;
    }

    if (ioret > 0) {
        event_set(&conn->event, sock, EV_WRITE,
                  (void *) client_process_data, conn);
        event_add(&conn->event, 0);
        return;
    }

    reset_iov(&conn->data);
    reset_query(&conn->query);

    /*
     * we've done all our work on this, go back to the beginning
     */
    event_set(&conn->event, sock, EV_READ,
              (void *) client_read_type, conn);
    event_add(&conn->event, 0);
}

void
client_read_payload(int sock, short which, client_conn_t * conn)
{
    int             ioret;

#if DEBUG
    LOG(logfile, "%d %d", conn->query.uri_len, conn->query.host_len);
#endif
    reset_connection_timeout(conn, connection_timeout);

    if (!conn->data.buf)
        initialize_iov(&conn->data,
                       conn->query.uri_len + conn->query.host_len);

    ioret = read_iov(&conn->data, sock);

    if (ioret < 0) {
        free_client_conn(conn);
        return;
    }

    if (ioret > 0) {
        event_set(&conn->event, sock, EV_READ,
                  (void *) client_read_payload, conn);
        event_add(&conn->event, 0);
        return;
    }

    conn->query.uri = calloc(conn->query.uri_len + 1, 1);

    if (!conn->query.uri) {
        LOG(logfile, "Out of memory: %s", strerror(errno));
        exit(1);
    }

    conn->query.host = calloc(conn->query.host_len + 1, 1);

    if (!conn->query.host) {
        LOG(logfile, "Out of memory: %s", strerror(errno));
        exit(1);
    }

    memcpy(conn->query.uri, conn->data.buf, conn->query.uri_len);

    memcpy(conn->query.host,
           &conn->data.buf[conn->query.uri_len], conn->query.host_len);

    reset_iov(&conn->data);

#ifdef DEBUG
    LOG(logfile, "host: '%s' uri: '%s'", conn->query.host,
        conn->query.uri);
#endif

    event_set(&conn->event, sock, EV_WRITE,
              (void *) client_process_data, conn);
    event_add(&conn->event, 0);

}

void
client_read_v3_header(int sock, short which, client_conn_t * conn)
{
    /*
     * version 3 header includes an extra 32 bit field at the start of
     * the packet. This allows a client to set an ID which will be echoed
     * along with the response. Otherwise it's just like v1
     */
    int             ioret;

    reset_connection_timeout(conn, connection_timeout);

    if (!conn->data.buf)
        initialize_iov(&conn->data, sizeof(uint32_t));

    ioret = read_iov(&conn->data, sock);

    if (ioret < 0) {
        free_client_conn(conn);
        return;
    }

    if (ioret > 0) {
        event_set(&conn->event, sock, EV_READ,
                  (void *) client_read_v3_header, conn);
        event_add(&conn->event, 0);
        return;
    }

    memcpy(&conn->id, conn->data.buf, sizeof(uint32_t));
    reset_iov(&conn->data);

#ifdef DEBUG
    LOG(logfile, "Got ident %u", ntohl(conn->id));
#endif

    /*
     * go back to reading a v1 like packet
     */
    event_set(&conn->event, sock, EV_READ,
              (void *) client_read_v1_header, conn);
    event_add(&conn->event, 0);
}

void
client_read_v2_header(int sock, short which, client_conn_t * conn)
{
    int             ioret;
    uint32_t        saddr;

    reset_connection_timeout(conn, connection_timeout);

    if (!conn->data.buf)
        initialize_iov(&conn->data, 4);

    ioret = read_iov(&conn->data, sock);

    if (ioret < 0) {
        free_client_conn(conn);
        return;
    }

    if (ioret > 0) {
        event_set(&conn->event, sock, EV_READ,
                  (void *) client_read_v2_header, conn);
        event_add(&conn->event, 0);
        return;
    }

    memcpy(&saddr, conn->data.buf, sizeof(uint32_t));
    conn->query.saddr = saddr;
    reset_iov(&conn->data);

    /*
     * v2 allows us to just recv a source address, thus we can go directly
     * into processing the data
     */
    event_set(&conn->event, sock, EV_WRITE,
              (void *) client_process_data, conn);
    event_add(&conn->event, 0);
}

void
client_read_v1_header(int sock, short which, client_conn_t * conn)
{
    int             ioret;
    uint32_t        saddr;
    uint16_t        urilen;
    uint16_t        hostlen;

    reset_connection_timeout(conn, connection_timeout);

    if (!conn->data.buf)
        initialize_iov(&conn->data, 8);

    ioret = read_iov(&conn->data, sock);

    if (ioret < 0) {
        free_client_conn(conn);
        return;
    }

    if (ioret > 0) {
        event_set(&conn->event, sock, EV_READ,
                  (void *) client_read_v1_header, conn);
        event_add(&conn->event, 0);
        return;
    }

    memcpy(&saddr, &conn->data.buf[0], sizeof(uint32_t));
    memcpy(&urilen, &conn->data.buf[4], sizeof(uint16_t));
    memcpy(&hostlen, &conn->data.buf[6], sizeof(uint16_t));

    urilen = ntohs(urilen);
    hostlen = ntohs(hostlen);

#ifdef DEBUG
    LOG(logfile, "saddr = %u", saddr);
    LOG(logfile, "ulen %d hlen %d", urilen, hostlen);
#endif

    if (urilen > MAX_URI_SIZE || hostlen > MAX_HOST_SIZE ||
        urilen <= 0 || hostlen <= 0) {
        free_client_conn(conn);
        return;
    }

    conn->query.uri_len = urilen;
    conn->query.host_len = hostlen;
    conn->query.saddr = saddr;
    reset_iov(&conn->data);

    event_set(&conn->event, sock, EV_READ,
              (void *) client_read_payload, conn);
    event_add(&conn->event, 0);
}

void
client_read_injection(int sock, short which, client_conn_t * conn)
{
    blocked_node_t *bnode;
    uint32_t        saddr;
    int             ioret;

    reset_connection_timeout(conn, connection_timeout);

    if (!conn->data.buf)
        initialize_iov(&conn->data, sizeof(uint32_t));

    ioret = read_iov(&conn->data, sock);

    if (ioret < 0) {
        free_client_conn(conn);
        return;
    }

    if (ioret > 0) {
        event_set(&conn->event, sock, EV_READ,
                  (void *) client_read_injection, conn);
        event_add(&conn->event, 0);
        return;
    }

    memcpy(&saddr, conn->data.buf, sizeof(uint32_t));

    switch (conn->type) {
    case TYPE_INJECT:
        /*
         * make sure the node doesn't already exist
         */
        if ((bnode = g_tree_lookup(current_blocks, &saddr))) {
            bnode->count++;
            total_blocked_connections++;
            break;
        }

        /*
         * this is starting to get a little hacky I think. We can re-factor
         * if I ever end up doing any other types of features
         */
        if (recently_blocked &&
            (bnode = g_tree_lookup(recently_blocked, &saddr)))
            /*
             * remove the bnode from the recently blocked list
             * so the bnode is now set to this instead of a new
             * allocd version
             */
        {
            g_tree_remove(recently_blocked, &saddr);
            /*
             * unset the recently_blocked timeout
             */
            evtimer_del(&bnode->recent_block_timeout);
        } else
            bnode = block_addr(NULL, saddr);

        bnode->count = 0;
        bnode->first_seen_addr = 0xffffffff;

        break;

    case TYPE_REMOVE:
        if (!(bnode = g_tree_lookup(current_blocks, &saddr)))
            break;
        expire_bnode(0, 0, bnode);
        break;
    }


    reset_iov(&conn->data);

    event_set(&conn->event, conn->sock, EV_READ,
              (void *) client_read_type, conn);
    event_add(&conn->event, 0);
}

void
client_read_type(int sock, short which, client_conn_t * conn)
{
    int             ioret;
    uint8_t         type;

    reset_connection_timeout(conn, connection_timeout);

    if (!conn->data.buf)
        initialize_iov(&conn->data, 1);

    ioret = read_iov(&conn->data, sock);

    if (ioret < 0) {
        free_client_conn(conn);
        return;
    }

    if (ioret > 0) {
        /*
         * what? can't get 1 byte? lame
         */
        free_client_conn(conn);
        return;
    }

    type = *conn->data.buf;
    conn->type = type;
    evutil_gettimeofday(&conn->last_time, NULL);
    conn->requests++;

#ifdef DEBUG
    LOG(logfile, "type %d", type);
#endif
    switch (type) {
    case TYPE_THRESHOLD_v1:
        /*
         * thresholding analysis with uri/host
         */
        event_set(&conn->event, sock, EV_READ,
                  (void *) client_read_v1_header, conn);
        event_add(&conn->event, 0);
        break;
    case TYPE_THRESHOLD_v2:
        /*
         * thresholding for IP analysis only
         */
        event_set(&conn->event, sock, EV_READ,
                  (void *) client_read_v2_header, conn);
        event_add(&conn->event, 0);
        break;
    case TYPE_THRESHOLD_v3:
        /*
         * just like v1 but with a 32bit identification header
         */
        event_set(&conn->event, sock, EV_READ,
                  (void *) client_read_v3_header, conn);
        event_add(&conn->event, 0);
        break;
    case TYPE_REMOVE:
    case TYPE_INJECT:
        event_set(&conn->event, sock, EV_READ,
                  (void *) client_read_injection, conn);
        event_add(&conn->event, 0);
        break;
    default:
        free_client_conn(conn);
        return;
    }

    reset_iov(&conn->data);
}

void
server_driver(int sock, short which, void *args)
{
    int             csock;
    struct sockaddr_in addr;
    socklen_t       addrlen;
    struct timeval  tv;

    addrlen = sizeof(struct sockaddr);
    csock = accept(sock, (struct sockaddr *) &addr, &addrlen);

    if (csock <= 0) {
        close(csock);
        return;
    }

    if (set_nb(csock)) {
        close(csock);
        return;
    }

    client_conn_t  *new_conn = calloc(sizeof(client_conn_t), 1);

    if (!new_conn) {
        LOG(logfile, "Out of memory: %s", strerror(errno));
        exit(1);
    }

    new_conn->sock = csock;
    new_conn->conn_addr = (uint32_t) addr.sin_addr.s_addr;
    new_conn->conn_port = (uint16_t) addr.sin_port;
    evutil_gettimeofday(&tv, NULL);
    new_conn->conn_time = tv.tv_sec;


    if (debug)
        LOG(logfile, "New connection from %s:%d",
            inet_ntoa(*(struct in_addr *) &new_conn->conn_addr),
            ntohs(new_conn->conn_port));

    current_connections_count++;
    current_connections =
        g_slist_prepend(current_connections, (gpointer) new_conn);

    event_set(&new_conn->event, csock, EV_READ,
              (void *) client_read_type, new_conn);
    event_add(&new_conn->event, 0);

    reset_connection_timeout(new_conn, connection_timeout);

    return;
}

int
server_init(void)
{
    struct sockaddr_in addr;
    int             sock,
                    v = 1;

    if ((sock = socket(PF_INET, SOCK_STREAM, 0)) <= 0)
        return -1;

    if ((setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                    (char *) &v, sizeof(v))) < 0)
        return -1;

    if (set_nb(sock))
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bind_port);
    addr.sin_addr.s_addr = inet_addr(bind_addr);

    if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0)
        return -1;

    if (listen(sock, 1024) < 0)
        return -1;

    event_set(&server_event, sock, EV_READ | EV_PERSIST,
              server_driver, NULL);
    event_add(&server_event, 0);

    return 0;
}

uint32_t
get_random_integer(void)
{
    return 4; // chosen by fair dice roll.
              // guaranteed to be random.
}

void
load_config(gboolean reload)
{
    GKeyFileFlags   flags;
    GKeyFile       *config_file;
    int             i;
    GError         *error = NULL;

    typedef enum {
        _c_f_t_str = 1,
        _c_f_t_int,
        _c_f_t_trie,
        _c_f_t_ratio,
        _c_f_t_file
    } _c_f_t;

    struct _c_f_in {
        char           *parent;
        char           *key;
        _c_f_t          type;
        void           *var;
        gboolean        reloadable;
    } c_f_in[] = {
        {"thrashd", "conn-timeout",               _c_f_t_int,   &connection_timeout,       TRUE},
        {"thrashd", "name",                       _c_f_t_str,   &process_name,             TRUE},
        {"thrashd", "uri-check",                  _c_f_t_int,   &uri_check,                TRUE},
        {"thrashd", "host-check",                 _c_f_t_int,   &host_check,               TRUE},
        {"thrashd", "addr-check",                 _c_f_t_int,   &addr_check,               TRUE},
        {"thrashd", "http-listen-port",           _c_f_t_int,   &server_port,              FALSE},
        {"thrashd", "listen-port",                _c_f_t_int,   &bind_port,                FALSE},
        {"thrashd", "listen-addr",                _c_f_t_str,   &bind_addr,                FALSE},
        {"thrashd", "block-timeout",              _c_f_t_int,   &soft_block_timeout,       TRUE},
        {"thrashd", "hard-timeout",               _c_f_t_int,   &hard_block_timeout,       TRUE},
        {"thrashd", "uri-ratio",                  _c_f_t_ratio, &uri_ratio,                TRUE},
        {"thrashd", "host-ratio",                 _c_f_t_ratio, &host_ratio,               TRUE},
        {"thrashd", "addr-ratio",                 _c_f_t_ratio, &addr_ratio,               TRUE},
        {"thrashd", "daemonize",                  _c_f_t_int,   &rundaemon,                FALSE},
        {"thrashd", "syslog",                     _c_f_t_int,   &syslog_enabled,           FALSE},
        {"thrashd", "syslog-facility",            _c_f_t_str,   &syslog_facility,          FALSE},
        {"thrashd", "rbl-zone",                   _c_f_t_str,   &rbl_zone,                 FALSE},
        {"thrashd", "rbl-negative-cache-timeout", _c_f_t_int,   &rbl_negcache_timeout,     TRUE},
        {"thrashd", "rbl-nameserver",             _c_f_t_str,   &rbl_ns,                   FALSE},
        {"thrashd", "rbl-max-query",              _c_f_t_int,   &rbl_max_queries,          TRUE},
        {"thrashd", "rand-ratio",                 _c_f_t_trie,  &recently_blocked,         FALSE},
        {"thrashd", "min-rand-ratio",             _c_f_t_ratio, &minimum_random_ratio,     TRUE},
        {"thrashd", "max-rand-ratio",             _c_f_t_ratio, &maximum_random_ratio,     TRUE},
        {"thrashd", "recently-blocked-timeout",   _c_f_t_int,   &recently_blocked_timeout, TRUE},
        {"thrashd", "user",                       _c_f_t_str,   &drop_user,                FALSE},
        {"thrashd", "group",                      _c_f_t_str,   &drop_group,               FALSE},
        {"thrashd", "logfile",                    _c_f_t_file,  &logfile,                  FALSE},
        {"thrashd", "velocity-num",               _c_f_t_int,   &velocity_num,             TRUE},
        {"thrashd", "http-password",              _c_f_t_str,   &http_password,            TRUE},
        {"thrashd", "backup-file",                _c_f_t_str,   &backup_file,              FALSE},
        {"thrashd", "debug",                      _c_f_t_int,   &debug,                    TRUE},
        {"thrashd", "geoip-file",                 _c_f_t_str,   &geoip_file,               FALSE},
        {"thrashd", "ip-action-url",              _c_f_t_str,   &ip_action_url,            TRUE},
#ifdef WITH_BGP
        {"thrashd", "bgp-sock",                   _c_f_t_str,   &bgp_sockname,             FALSE},
#endif
        {NULL, NULL, 0, NULL, 0}
    };

    config_file = g_key_file_new();

    flags = G_KEY_FILE_KEEP_COMMENTS;

    if (reload)
        LOG(logfile, "Reloading config: %s", config_filename);

    if (!g_key_file_load_from_file(config_file, config_filename, flags, &error)) {
        LOG(logfile, "Error loading config: %s", error->message);
        if (reload)
            return;
        else
            exit(1);
    }

    if (g_key_file_has_group(config_file, "uri-ratios")) {
        gsize length;
        gchar **keys = g_key_file_get_keys (config_file, "uri-ratios", &length, &error);
        if (error) {
            LOG(logfile, "Error with uri-ratios: %s", error->message);
            if (reload)
                return;
            else
                exit(1);
        } else if (length && keys) {
            if (uris_ratio_table)
                g_hash_table_destroy(uris_ratio_table);

            uris_ratio_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
            for (i = 0 ; i < length; i++) {
                block_ratio_t *ratio = g_malloc(sizeof(block_ratio_t));
                char *str = g_key_file_get_string(config_file,
                                                  "uri-ratios",
                                                  keys[i], NULL);
                if (str) {
                    gchar **splitter = g_strsplit(str, ":", 2);
                    if (splitter && splitter[0] && splitter[1]) {
                        ratio->num_connections = atoll(splitter[0]);
                        ratio->timelimit = atoll(splitter[1]);
                        g_hash_table_insert(uris_ratio_table, keys[i], ratio);
                        g_strfreev(splitter);
                    } else {
                        LOG(logfile, "Bad uri-ratios: %s", str);
                        exit(1);
                    }
                    g_free(str);
                }
            }
            /* Do NOT free keys since g_hash_table_insert retains it */
        }
    }


    for (i = 0; c_f_in[i].parent != NULL; i++) {
        char          **svar;
        char           *str;
        FILE          **fvar;
        int            *ivar;
        GTree         **tvar;
        block_ratio_t  *ratio;
        gchar         **splitter;
	gboolean        boolean;

        if (!g_key_file_has_key(config_file,
                                c_f_in[i].parent, c_f_in[i].key, &error))
            continue;

        if (reload && !c_f_in[i].reloadable)
            continue;

        switch (c_f_in[i].type) {

        case _c_f_t_str:
            svar = (char **) c_f_in[i].var;
            *svar = g_key_file_get_string(config_file,
                                          c_f_in[i].parent,
                                          c_f_in[i].key, NULL);
            break;
        case _c_f_t_trie:
	    tvar = (GTree **) c_f_in[i].var;

	    boolean =
		g_key_file_get_boolean(config_file,
			c_f_in[i].parent,
			c_f_in[i].key, NULL);

            if (boolean == TRUE)
                *tvar = g_tree_new((GCompareFunc) uint32_cmp);

            break;
        case _c_f_t_int:
            ivar = (int *) c_f_in[i].var;
            *ivar = g_key_file_get_integer(config_file,
                                           c_f_in[i].parent,
                                           c_f_in[i].key, NULL);
            break;
        case _c_f_t_ratio:
            ratio = (block_ratio_t *) c_f_in[i].var;
            str = g_key_file_get_string(config_file,
                                        c_f_in[i].parent,
                                        c_f_in[i].key, NULL);
            splitter = g_strsplit(str, ":", 2);
            if (splitter && splitter[0] && splitter[1]) {
                ratio->num_connections = atoll(splitter[0]);
                ratio->timelimit = atoll(splitter[1]);
                g_strfreev(splitter);
                g_free(str);
            } else {
                LOG(logfile, "Bad ratio %s for %s", str, c_f_in[i].key);
                exit(1);
            }
            break;
        case _c_f_t_file:
            fvar = (FILE **) c_f_in[i].var;
            str = g_key_file_get_string(config_file,
                                        c_f_in[i].parent,
                                        c_f_in[i].key, NULL);

            if (!(*fvar = fopen(str, "a+"))) {
                fprintf(stderr, "Could not open logfile %s\n",
                        strerror(errno));
                exit(1);
            }
            g_free(str);
            break;
        }
    }

    /* if random ratios are turned on, yet we don't have
       the minimum configured, lets throw up an error. */
    if (recently_blocked)
    {
	if ((!minimum_random_ratio.num_connections &&
	     !minimum_random_ratio.timelimit) ||
	    (!maximum_random_ratio.num_connections &&
	     !minimum_random_ratio.timelimit))
	{
	    LOG(logfile,
		    "Recently blocked (%p) Enabled without max-rand-ratio or"
		    " min-rand-ratio configured!", recently_blocked);
	    exit(1);
	}

	if (!recently_blocked_timeout)
	{
	    LOG(logfile,
		    "Recently blocked (%p) is enabled without "
		    "recently-blocked-timeout configured", recently_blocked);
	    exit(1);
	}
    }

    g_key_file_free(config_file);
}


int
parse_args(int argc, char **argv)
{
    extern char    *optarg;
    extern int      optind,
                    opterr,
                    optopt;
    int             c;

    static char    *help =
        "Copyright AOL LLC 2008-2009\n\n"
        "Options: \n"
        "   -h:        Help me!!\n"
        "   -v:        Version\n"
        "   -d:        Debug\n"
        "   -c <file>: Configuration file\n";

    while ((c = getopt(argc, argv, "dhvc:")) != -1) {
        switch (c) {
        case 'c':
            config_filename = optarg;
            load_config(FALSE);
            break;
        case 'v':
            printf("%s (%s)\n", VERSION, VERSION_NAME);
            exit(1);
        case 'd':
            debug = 1;
            break;
        case 'h':
            printf("Usage: %s [opts]\n%s", argv[0], help);
            exit(1);
        default:
            printf("Unknown option %s\n", optarg);
            exit(1);
        }
    }

    return 0;
}

void
qps_init(void)
{
    struct timeval  tv;

    tv.tv_sec = 1;
    tv.tv_usec = 0;

    evtimer_set(&qps_event, (void *) qps_reset, NULL);
    evtimer_add(&qps_event, &tv);
}

void
qps_reset(int sock, int which, void *args)
{
    qps_last = qps;
    qps = 0;

    qps_init();
}

static struct dsn_c_pvt_sfnt {
    int             val;
    const char     *strval;
} facilities[] = {
    {
    LOG_KERN, "kern"}, {
    LOG_USER, "user"}, {
    LOG_MAIL, "mail"}, {
    LOG_DAEMON, "daemon"}, {
    LOG_AUTH, "auth"}, {
    LOG_SYSLOG, "syslog"}, {
    LOG_LPR, "lpr"}, {
    LOG_NEWS, "news"}, {
    LOG_UUCP, "uucp"}, {
    LOG_CRON, "cron"}, {
    LOG_AUTHPRIV, "authpriv"}, {
    LOG_FTP, "ftp"}, {
    LOG_LOCAL0, "local0"}, {
    LOG_LOCAL1, "local1"}, {
    LOG_LOCAL2, "local2"}, {
    LOG_LOCAL3, "local3"}, {
    LOG_LOCAL4, "local4"}, {
    LOG_LOCAL5, "local5"}, {
    LOG_LOCAL6, "local6"}, {
    LOG_LOCAL7, "local7"}, {
    0, NULL}
};

void
syslog_init(char *facility)
{
    int             i;
    int             facility_num = 0;
    char           *ident;

    if (!syslog_enabled)
        return;

    for (i = 0; facilities[i].strval != NULL; i++) {
        if (strcasecmp(facilities[i].strval, facility) == 0) {
            facility_num = facilities[i].val;
            break;
        }
    }

    if (!facility_num) {
        syslog_enabled = 0;
        fprintf(stderr, "No valid facility, syslog will be turned off\n");
        return;
    }

    if (!(ident = malloc(255))) {
        LOG(logfile, "Out of memory: %s", strerror(errno));
        exit(1);
    }

    snprintf(ident, 254, "thrashd-%s", process_name);

    openlog(ident, 0, facility_num);
}

void
rbl_init(void)
{
    if (!rbl_zone)
        return;

    if (rbl_ns)
        evdns_nameserver_ip_add(rbl_ns);
    else
        evdns_resolv_conf_parse(DNS_OPTION_NAMESERVERS,
                                "/etc/resolv.conf");

    evdns_set_option("timeout:", "1", DNS_OPTIONS_ALL);
    evdns_set_option("max-timeouts:", "3", DNS_OPTIONS_ALL);

    if (evdns_count_nameservers() <= 0) {
        LOG(logfile, "Couldn't setup RBL server! %s", "");

        exit(1);
    }

    LOG(logfile, "RBL Zone '%s' initialized", rbl_zone);
}

void
daemonize(const char *path)
{
    int             status;
    int             fd;

    status = fork();
    if (status < 0) {
        fprintf(stderr, "Can't fork!\n");
        exit(1);
    }

    else if (status > 0)
        _exit(0);

#if HAVE_SETSID
    assert(setsid() >= 0);
#elif defined(TIOCNOTTY)
    fd = open("/dev/tty", O_RDWR);

    if (fd >= 0)
        assert(ioctl(fd, TIOCNOTTY, NULL) >= 0);
#endif

    assert(chdir(path) >= 0);

    fd = open("/dev/null", O_RDWR, 0);

    if (fd != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2)
            close(fd);
    }
}

void
log_startup(void)
{
    LOG(logfile, "Name:             %s", process_name);

    if (uri_check) {
        LOG(logfile, "URI Block Ratio:  %u connections within %u seconds",
            uri_ratio.num_connections, uri_ratio.timelimit);
    } else {
        LOG(logfile, "URI Block:        DISABLED%s", "");
    }

    if (host_check) {
        LOG(logfile, "Host Block Ratio: %u connections within %u seconds",
            host_ratio.num_connections, host_ratio.timelimit);
    } else {
        LOG(logfile, "Host Block:       DISABLED%s", "");
    }

    if (addr_check) {
        LOG(logfile, "Addr Block Ratio: %u connections within %u seconds",
            addr_ratio.num_connections, addr_ratio.timelimit);
    } else {
        LOG(logfile, "Host Block:       DISABLED%s", "");
    }

    LOG(logfile, "HTTP Listen Port:   %d", server_port);
    LOG(logfile, "Bind addr:          %s", bind_addr);
    LOG(logfile, "Listen Port:        %d", bind_port);
    LOG(logfile, "Block Timeout:      %d", soft_block_timeout);
    LOG(logfile, "Hard Block Timeout: %d", hard_block_timeout);

    if (rbl_zone) {
        LOG(logfile, "RBL:                  ENABLED%s", "");
        LOG(logfile, "RBL Zone:             %s", rbl_zone);
        LOG(logfile, "RBL Nameserver:       %s", rbl_ns);
        LOG(logfile, "RBL Negative Timeout: %d", rbl_negcache_timeout);
        LOG(logfile, "RBL Max Queries:      %d", rbl_max_queries);
    } else {
        LOG(logfile, "RBL:              DISABLED%s", "");
    }


}


gboolean save_data_current(gpointer key, blocked_node_t *bn, FILE *file)
{
    FEXPORT_u32(file, bn->saddr);
    FEXPORT_u32(file, bn->count);
    FEXPORT_u32(file, bn->last_time.tv_sec);
    FEXPORT_u32(file, bn->first_seen_addr);
    FEXPORT_u16(file, bn->ratio.num_connections);
    FEXPORT_u16(file, bn->ratio.timelimit);
    if (bn->timeout.ev_timeout.tv_sec == 0) {
        FEXPORT_u32(file, 0);
    } else {
        FEXPORT_u32(file, event_remaining_seconds(&bn->timeout));
    }
    if (bn->hard_timeout.ev_timeout.tv_sec == 0) {
        FEXPORT_u32(file, 0);
    } else {
        FEXPORT_u32(file, event_remaining_seconds(&bn->hard_timeout));
    }
    return FALSE;
}


void
save_data ()
{
    FILE            *file;
    struct timeval  current_time;

    if (!backup_file)
        return;

    evutil_gettimeofday(&current_time, NULL);

    if (!(file = fopen(backup_file, "w"))) {
        LOG(logfile, "ERROR: Backup file (%s) couldn't be openned: %s", backup_file, strerror(errno));
        return;
    }
    FEXPORT_str(file, "thrasher");
    FEXPORT_u16(file, 1); /* version */
    FEXPORT_u32(file, current_time.tv_sec);

    FEXPORT_u32(file, g_tree_nnodes(current_blocks));
    g_tree_foreach(current_blocks, (GTraverseFunc)save_data_current, file);

    LOG(logfile, "SUCCESS: Saving backup data%s", "");
    fclose(file);
    return;
}

void save_data_cb(int sock, int which, void *args);

void
save_data_init(void)
{
    struct timeval  tv;

    tv.tv_sec = 5*60;
    tv.tv_usec = 0;

    evtimer_set(&backup_event, (void *) save_data_cb, NULL);
    evtimer_add(&backup_event, &tv);
}

void
save_data_cb(int sock, int which, void *args)
{
    save_data();
    save_data_init();
}

void
load_data ()
{
    FILE          *file;
    char          *str;
    int            len;
    uint16_t       version;
    uint32_t       file_time;
    int32_t        time_diff;
    struct timeval current_time;
    uint32_t       num;

    save_data_init();

    if (!(file = fopen(backup_file, "r"))) {
        LOG(logfile, "ERROR: Backup file (%s) couldn't be openned: %s", backup_file, strerror(errno));
        return;
    }

    evutil_gettimeofday(&current_time, NULL);

    FIMPORT_str(file, str, len);
    FIMPORT_u16(file, version);
    if (version != 1 || len != 8 || memcmp(str, "thrasher", len) != 0) {
        fclose(file);
        LOG(logfile, "ERROR: Backup file corrupt%s", "");
        free(str);
        return;
    }
    free(str);

    FIMPORT_u32(file, file_time);
    time_diff = current_time.tv_sec - file_time;

    FIMPORT_u32(file, num);
    while (!feof(file) && num > 0) {
        int32_t         soft_timeout, hard_timeout;
        blocked_node_t *bn;
        struct timeval  tv;

        if (!(bn = malloc(sizeof(blocked_node_t)))) {
            LOG(logfile, "Out of memory: %s", strerror(errno));
            exit(1);
        }
        memset(bn, 0, sizeof(blocked_node_t));

        FIMPORT_u32(file, bn->saddr);
        FIMPORT_u32(file, bn->count);
        FIMPORT_u32(file, bn->last_time.tv_sec);
        FIMPORT_u32(file, bn->first_seen_addr);
        FIMPORT_u16(file, bn->ratio.num_connections);
        FIMPORT_u16(file, bn->ratio.timelimit);
        FIMPORT_u32(file, soft_timeout);
        FIMPORT_u32(file, hard_timeout);

        if (feof(file)) {
            free(bn);
            break;
        }


        if (soft_timeout <= time_diff) {
            free(bn);
            continue;
        }

        tv.tv_sec = soft_timeout - time_diff;
        tv.tv_usec = 0;

        g_tree_insert(current_blocks, &bn->saddr, bn);

        evtimer_set(&bn->timeout, (void *) expire_bnode, bn);
        evtimer_add(&bn->timeout, &tv);

        if (hard_timeout > 0) {
            tv.tv_sec  = hard_timeout - time_diff;
            tv.tv_usec = 0;

            evtimer_set(&bn->hard_timeout, (void *)expire_hard_bnode, bn);
            evtimer_add(&bn->hard_timeout, &tv);
        }

        num--;
    }

    fclose(file);
}

#ifdef ARCH_LINUX
void
segvfunc(int sig)
{
    int             c,
                    i;
    void           *funcs[128];
    char          **names;

    c = backtrace(funcs, 128);
    names = backtrace_symbols(funcs, c);

    for (i = 0; i < c; i++)
        LOG(logfile, "%s", names[i]);

    free(names);

    signal(sig, SIG_DFL);
    kill(getpid(), sig);
}
#endif

void
geoip_init(void)
{

#ifdef WITH_GEOIP
    if (!geoip_file)
        return;

    gi = GeoIP_open(geoip_file, GEOIP_MEMORY_CACHE);
    if (!gi) {
        LOG(logfile, "ERROR: Couldn't initialize GeoIP %s from %s", strerror(errno), geoip_file);
        exit(1);
    }
#endif
}

void
bgp_init(void)
{
#ifdef WITH_BGP
    if (!bgp_sockname)
        return;

    if ((bgp_sock = thrash_bgp_connect(bgp_sockname)) < 0) {
        LOG(logfile, "ERROR: bgpd sock: %s", strerror(errno));
        exit(1);
    }
    LOG(logfile, "sock %d", bgp_sock);

#endif
}

void
thrashd_init(void)
{
    struct sigaction sa;

    rbl_init();
    qps_init();
    randdata = g_rand_new();

    if (webserver_init() == -1) {
        LOG(logfile, "ERROR: Could not bind webserver port: %s",
            strerror(errno));
        exit(1);
    }

    if (server_init() == -1) {
        LOG(logfile, "ERROR: Could not bind to port: %s", strerror(errno));
        exit(1);
    }

    syslog_init(syslog_facility);

#ifdef ARCH_LINUX
    signal(SIGSEGV, segvfunc);
#endif

    /*
     * ignore sigpipe, it's annoying HEYOOOO
     */
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;

    if (sigemptyset(&sa.sa_mask) == -1 || sigaction(SIGPIPE, &sa, 0) == -1) {
        LOG(logfile,
            "ERROR: failed to ignore SIGPIPE: %s", strerror(errno));
        exit(1);
    }


}

int
drop_perms(void)
{
    struct passwd  *usr;
    struct group   *grp;
    if (drop_group) {
        grp = getgrnam(drop_group);

        if (!grp) {
            LOG(logfile, "ERROR: group %s not found.", drop_group);
            exit(1);
        }

        if (setgid(grp->gr_gid) != 0) {
            LOG(logfile, "ERROR: setgid failed %s", strerror(errno));
            exit(1);
        }

    }

    if (drop_user) {
        usr = getpwnam(drop_user);
        if (!usr) {
            LOG(logfile, "ERROR: User %s not found.", drop_user);
            exit(1);
        }
        if (seteuid(usr->pw_uid) != 0) {
            LOG(logfile, "ERROR: setuid failed %s", strerror(errno));
            exit(1);
        }
    }
    return 0;
}

int
main(int argc, char **argv)
{
    increase_limits();
    globals_init();
    parse_args(argc, argv);
    event_init();
    geoip_init();
    thrashd_init();
    bgp_init();
    log_startup();
    drop_perms();

    if (backup_file)
        load_data();

    if (rundaemon)
        daemonize("/tmp");

    event_loop(0);

    return 0;
}
