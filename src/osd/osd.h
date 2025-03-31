// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.1 (see README.md for details)

#pragma once

#include <sys/types.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <arpa/inet.h>
#include <malloc.h>

#include <set>
#include <deque>

#include "blockstore.h"
#include "ringloop.h"
#include "timerfd_manager.h"
#include "epoll_manager.h"
#include "osd_peering_pg.h"
#include "messenger.h"
#include "etcd_state_client.h"

#define OSD_LOADING_PGS 0x01
#define OSD_PEERING_PGS 0x04
#define OSD_FLUSHING_PGS 0x08
#define OSD_RECOVERING 0x10
#define OSD_SCRUBBING 0x20

#define MAX_AUTOSYNC_INTERVAL 3600
#define DEFAULT_AUTOSYNC_INTERVAL 5
#define DEFAULT_AUTOSYNC_WRITES 128
#define MAX_RECOVERY_QUEUE 2048
#define DEFAULT_RECOVERY_QUEUE 1
#define DEFAULT_RECOVERY_PG_SWITCH 128
#define DEFAULT_RECOVERY_BATCH 16

//#define OSD_STUB

struct osd_object_id_t
{
    osd_num_t osd_num;
    object_id oid;
};

struct osd_recovery_op_t
{
    int st = 0;
    bool degraded = false;
    object_id oid = { 0 };
    osd_op_t *osd_op = NULL;
};

// Posted as /osd/inodestats/$osd, then accumulated by the monitor
#define INODE_STATS_READ 0
#define INODE_STATS_WRITE 1
#define INODE_STATS_DELETE 2
struct inode_stats_t
{
    uint64_t op_sum[3] = { 0 };
    uint64_t op_count[3] = { 0 };
    uint64_t op_bytes[3] = { 0 };
};

struct bitmap_request_t
{
    osd_num_t osd_num;
    object_id oid;
    uint64_t version;
    void *bmp_buf;
};

inline bool operator < (const bitmap_request_t & a, const bitmap_request_t & b)
{
    return a.osd_num < b.osd_num || a.osd_num == b.osd_num && a.oid < b.oid;
}

struct osd_chain_read_t
{
    int chain_pos;
    inode_t inode;
    uint32_t offset, len;
};

struct osd_rmw_stripe_t;

struct recovery_stat_t
{
    uint64_t count, usec, bytes;
};

class osd_t
{
    // config

    json11::Json::object cli_config, file_config, etcd_global_config, etcd_osd_config, config;
    int etcd_report_interval = 5;
    int etcd_stats_interval = 30;

    bool readonly = false;
    osd_num_t osd_num = 1; // OSD numbers start with 1
    bool run_primary = false;
    bool no_rebalance = false;
    bool no_recovery = false;
    bool no_scrub = false;
    bool allow_net_split = false;
    std::vector<std::string> cfg_bind_addresses;
    int bind_port, listen_backlog = 128;
    bool use_rdmacm = false;
    bool disable_tcp = false;
    // FIXME: Implement client queue depth limit
    int client_queue_depth = 128;
    bool allow_test_ops = false;
    int print_stats_interval = 3;
    int slow_log_interval = 10;
    int immediate_commit = IMMEDIATE_NONE;
    int autosync_interval = DEFAULT_AUTOSYNC_INTERVAL; // "emergency" sync every 5 seconds
    int autosync_writes = DEFAULT_AUTOSYNC_WRITES;
    uint64_t recovery_queue_depth = 1;
    uint64_t recovery_sleep_us = 0;
    double recovery_tune_util_low = 0.1;
    double recovery_tune_client_util_low = 0;
    double recovery_tune_util_high = 1.0;
    double recovery_tune_client_util_high = 0.5;
    int recovery_tune_interval = 1;
    int recovery_tune_agg_interval = 10;
    int recovery_tune_sleep_min_us = 10;
    int recovery_tune_sleep_cutoff_us = 10000000;
    int recovery_pg_switch = DEFAULT_RECOVERY_PG_SWITCH;
    int recovery_sync_batch = DEFAULT_RECOVERY_BATCH;
    int inode_vanish_time = 60;
    int log_level = 0;
    bool auto_scrub = false;
    uint64_t global_scrub_interval = 30*86400;
    uint64_t scrub_queue_depth = 1;
    uint64_t scrub_sleep_ms = 0;
    uint32_t scrub_list_limit = 1000;
    bool scrub_find_best = true;
    uint64_t scrub_ec_max_bruteforce = 100;

    // cluster state

    etcd_state_client_t st_cli;
    osd_messenger_t msgr;
    int etcd_failed_attempts = 0;
    std::string etcd_lease_id;
    json11::Json self_state;
    bool loading_peer_config = false;
    std::set<pool_pg_num_t> pg_state_dirty;
    bool pg_config_applied = false;
    bool etcd_reporting_pg_state = false;
    bool etcd_reporting_stats = false;
    int print_stats_timer_id = -1, slow_log_timer_id = -1;
    uint64_t cur_slow_op_primary = 0;
    uint64_t cur_slow_op_secondary = 0;

    // peers and PGs

    std::map<pool_id_t, pg_num_t> pg_counts;
    std::map<pool_pg_num_t, pg_t> pgs;
    std::set<pool_pg_num_t> dirty_pgs;
    std::set<osd_num_t> dirty_osds;
    int copies_to_delete_after_sync_count = 0;
    uint64_t misplaced_objects = 0, degraded_objects = 0, incomplete_objects = 0, inconsistent_objects = 0, corrupted_objects = 0;
    int peering_state = 0;
    std::map<object_id, osd_recovery_op_t> recovery_ops;
    std::map<object_id, osd_op_t*> scrub_ops;
    bool recovery_last_degraded = true;
    pool_pg_num_t recovery_last_pg;
    object_id recovery_last_oid;
    int recovery_pg_done = 0, recovery_done = 0;
    osd_op_t *autosync_op = NULL;
    int autosync_copies_to_delete = 0;
    int autosync_timer_id = -1;

    // Scrubbing
    uint64_t scrub_nearest_ts = 0;
    int scrub_timer_id = -1;
    pool_pg_num_t scrub_last_pg = {};
    osd_op_t *scrub_list_op = NULL;
    pg_list_result_t scrub_cur_list = {};
    uint64_t scrub_list_pos = 0;

    // Unstable writes
    uint64_t unstable_write_count = 0;
    std::map<osd_object_id_t, uint64_t> unstable_writes;
    std::deque<osd_op_t*> syncs_in_progress;

    // client & peer I/O

    bool stopping = false;
    int inflight_ops = 0;
    blockstore_t *bs = NULL;
    void *zero_buffer = NULL;
    uint64_t zero_buffer_size = 0;
    uint32_t bs_block_size, bs_bitmap_granularity, clean_entry_bitmap_size;
    ring_loop_t *ringloop;
    timerfd_manager_t *tfd = NULL;
    epoll_manager_t *epmgr = NULL;

    int listening_port = 0;
    std::vector<std::string> bind_addresses;
    std::vector<int> listen_fds;
#ifdef WITH_RDMACM
    std::vector<rdma_cm_id *> rdmacm_listeners;
#endif
    ring_consumer_t consumer;

    // op statistics
    osd_op_stats_t prev_stats, prev_report_stats;
    timespec report_stats_ts;
    std::map<uint64_t, inode_stats_t> inode_stats;
    std::map<uint64_t, timespec> vanishing_inodes;
    const char* recovery_stat_names[2] = { "degraded", "misplaced" };
    recovery_stat_t recovery_stat[2];
    recovery_stat_t recovery_print_prev[2];
    recovery_stat_t recovery_report_prev[2];

    // recovery auto-tuning
    int rtune_timer_id = -1;
    uint64_t rtune_avg_lat = 0;
    double rtune_client_util = 0, rtune_target_util = 1;
    osd_op_stats_t rtune_prev_stats, rtune_prev_recovery_stats;
    std::vector<uint64_t> recovery_target_sleep_items;
    uint64_t recovery_target_sleep_us = 0;
    uint64_t recovery_target_sleep_total = 0;
    int recovery_target_sleep_cur = 0, recovery_target_sleep_count = 0;

    // cluster connection
    void parse_config(bool init);
    void init_cluster();
    void on_change_osd_state_hook(osd_num_t peer_osd);
    void on_change_backfillfull_hook(pool_id_t pool_id);
    void on_change_pg_history_hook(pool_id_t pool_id, pg_num_t pg_num);
    void on_change_etcd_state_hook(std::map<std::string, etcd_kv_t> & changes);
    void on_load_config_hook(json11::Json::object & changes);
    void on_reload_config_hook(json11::Json::object & changes);
    json11::Json on_load_pgs_checks_hook();
    void on_load_pgs_hook(bool success);
    void bind_socket();
    void acquire_lease();
    json11::Json get_osd_state();
    void create_osd_state();
    void renew_lease(bool reload);
    void print_stats();
    void tune_recovery();
    void apply_recovery_tune_interval();
    void print_slow();
    json11::Json get_statistics();
    void report_statistics();
    void report_pg_state(pg_t & pg);
    void report_pg_states();
    void apply_no_inode_stats();
    void apply_pg_count();
    void apply_pg_config();

    // event loop, socket read/write
    void loop();

    // peer handling (primary OSD logic)
    void parse_test_peer(std::string peer);
    void handle_peers();
    bool check_peer_config(osd_client_t *cl, json11::Json conf);
    void repeer_pgs(osd_num_t osd_num);
    void start_pg_peering(pg_t & pg);
    void drop_dirty_pg_connections(pool_pg_num_t pg);
    void submit_list_subop(osd_num_t role_osd, pg_peering_state_t *ps);
    void discard_list_subop(osd_op_t *list_op);
    bool stop_pg(pg_t & pg);
    void reset_pg(pg_t & pg);
    void finish_stop_pg(pg_t & pg);

    // flushing, recovery and backfill
    void submit_pg_flush_ops(pg_t & pg);
    void handle_flush_op(bool rollback, pool_id_t pool_id, pg_num_t pg_num, pg_flush_batch_t *fb, osd_num_t peer_osd, int retval);
    bool submit_flush_op(pool_id_t pool_id, pg_num_t pg_num, pg_flush_batch_t *fb, bool rollback, osd_num_t peer_osd, int count, obj_ver_id *data);
    bool pick_next_recovery(osd_recovery_op_t &op);
    void submit_recovery_op(osd_recovery_op_t *op);
    void finish_recovery_op(osd_recovery_op_t *op);
    bool continue_recovery();
    pg_osd_set_state_t* change_osd_set(pg_osd_set_state_t *st, pg_t *pg);

    // scrub
    void scrub_list(pool_pg_num_t pg_id, osd_num_t role_osd, object_id min_oid);
    int pick_next_scrub(object_id & next_oid);
    void submit_scrub_op(object_id oid);
    bool continue_scrub();
    void submit_scrub_subops(osd_op_t *cur_op);
    void scrub_check_results(osd_op_t *cur_op);
    void plan_scrub(pg_t & pg, bool report_state = true);
    void schedule_scrub(pg_t & pg);

    // op execution
    void exec_op(osd_op_t *cur_op);
    void finish_op(osd_op_t *cur_op, int retval);

    // secondary ops
    void exec_sync_stab_all(osd_op_t *cur_op);
    void exec_show_config(osd_op_t *cur_op);
    void exec_secondary(osd_op_t *cur_op);
    void exec_secondary_real(osd_op_t *cur_op);
    void secondary_op_callback(osd_op_t *cur_op);

    // primary ops
    void autosync();
    bool prepare_primary_rw(osd_op_t *cur_op);
    void continue_primary_read(osd_op_t *cur_op);
    void continue_primary_scrub(osd_op_t *cur_op);
    void continue_primary_describe(osd_op_t *cur_op);
    void continue_primary_list(osd_op_t *cur_op);
    void continue_primary_write(osd_op_t *cur_op);
    void cancel_primary_write(osd_op_t *cur_op);
    void continue_primary_sync(osd_op_t *cur_op);
    void continue_primary_del(osd_op_t *cur_op);
    bool check_write_queue(osd_op_t *cur_op, pg_t & pg);
    pg_osd_set_state_t* add_object_to_set(pg_t & pg, const object_id oid, const pg_osd_set_t & osd_set,
        uint64_t old_pg_state, int log_at_level);
    void remove_object_from_state(object_id & oid, pg_osd_set_state_t **object_state, pg_t &pg, bool report = true);
    pg_osd_set_state_t *mark_object(pg_t & pg, object_id oid, pg_osd_set_state_t *prev_object_state, bool ref,
        std::function<int(pg_osd_set_t & new_set)> calc_set);
    pg_osd_set_state_t *mark_object_corrupted(pg_t & pg, object_id oid, pg_osd_set_state_t *prev_object_state,
        osd_rmw_stripe_t *stripes, bool ref);
    pg_osd_set_state_t *mark_partial_write(pg_t & pg, object_id oid, pg_osd_set_state_t *prev_object_state,
        osd_rmw_stripe_t *stripes, bool ref);
    void deref_object_state(pg_t & pg, pg_osd_set_state_t **object_state, bool deref);
    bool remember_unstable_write(osd_op_t *cur_op, pg_t & pg, pg_osd_set_t & loc_set, int base_state);
    void handle_primary_subop(osd_op_t *subop, osd_op_t *cur_op);
    void handle_primary_bs_subop(osd_op_t *subop);
    void add_bs_subop_stats(osd_op_t *subop, bool recovery_related = false);
    void pg_cancel_write_queue(pg_t & pg, osd_op_t *first_op, object_id oid, int retval);

    void submit_primary_subops(int submit_type, uint64_t op_version, const uint64_t* osd_set, osd_op_t *cur_op);
    int submit_primary_subop_batch(int submit_type, inode_t inode, uint64_t op_version,
        osd_rmw_stripe_t *stripes, const uint64_t* osd_set, osd_op_t *cur_op, int subop_idx, int zero_read);
    void submit_primary_subop(osd_op_t *cur_op, osd_op_t *subop,
        osd_rmw_stripe_t *si, bool wr, inode_t inode, uint64_t op_version);
    void submit_primary_del_subops(osd_op_t *cur_op, uint64_t *cur_set, uint64_t set_size, pg_osd_set_t & loc_set);
    void submit_primary_del_batch(osd_op_t *cur_op, obj_ver_osd_t *chunks_to_delete, int chunks_to_delete_count);
    int submit_primary_sync_subops(osd_op_t *cur_op);
    void submit_primary_stab_subops(osd_op_t *cur_op);
    void submit_primary_rollback_subops(osd_op_t *cur_op, const uint64_t* osd_set);

    uint64_t* get_object_osd_set(pg_t &pg, object_id &oid, pg_osd_set_state_t **object_state);

    void continue_chained_read(osd_op_t *cur_op);
    int submit_chained_read_requests(pg_t & pg, osd_op_t *cur_op);
    void check_corrupted_chained(pg_t & pg, osd_op_t *cur_op);
    void send_chained_read_results(pg_t & pg, osd_op_t *cur_op);
    std::vector<osd_chain_read_t> collect_chained_read_requests(osd_op_t *cur_op);
    int collect_bitmap_requests(osd_op_t *cur_op, pg_t & pg, std::vector<bitmap_request_t> & bitmap_requests);
    int submit_bitmap_subops(osd_op_t *cur_op, pg_t & pg);
    int read_bitmaps(osd_op_t *cur_op, pg_t & pg, int base_state);

    inline pg_num_t map_to_pg(object_id oid, uint64_t pg_stripe_size)
    {
        uint64_t pg_count = pg_counts[INODE_POOL(oid.inode)];
        if (!pg_count)
            pg_count = 1;
        return (oid.stripe / pg_stripe_size) % pg_count + 1;
    }

public:
    osd_t(const json11::Json & config, ring_loop_t *ringloop);
    ~osd_t();
    void force_stop(int exitcode);
    bool shutdown();
};

inline bool operator == (const osd_object_id_t & a, const osd_object_id_t & b)
{
    return a.osd_num == b.osd_num && a.oid.inode == b.oid.inode && a.oid.stripe == b.oid.stripe;
}

inline bool operator < (const osd_object_id_t & a, const osd_object_id_t & b)
{
    return a.osd_num < b.osd_num || a.osd_num == b.osd_num && (
        a.oid.inode < b.oid.inode || a.oid.inode == b.oid.inode && a.oid.stripe < b.oid.stripe
    );
}
