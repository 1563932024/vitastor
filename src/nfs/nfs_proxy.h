// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.1 (see README.md for details)
//
// Simplified NFS proxy - main entrypoint header

#pragma once

#include "cluster_client.h"
#include "epoll_manager.h"
#include "nfs_portmap.h"
#include "proto/xdr_impl.h"
#include "vitastor_kv.h"

#define NFS_ROOT_HANDLE "R"
#define RPC_INIT_BUF_SIZE 32768
#define MAX_REQUEST_SIZE 128*1024*1024
#define TRUE 1
#define FALSE 0

class cli_tool_t;

struct kv_fs_state_t;
struct block_fs_state_t;

class nfs_proxy_t
{
public:
    std::string bind_address;
    uint64_t fsid = 1;
    uint64_t server_id = 0;
    // FIXME: Maybe allow to create files in different pools?
    std::string default_pool;
    std::string export_root;
    bool portmap_enabled;
    unsigned nfs_port;
    int trace = 0;
    std::string logfile = "/dev/null";
    std::string pidfile;
    bool exit_on_umount = false;
    std::string mountpoint;
    std::string mountopts;
    std::string fsname;

    int active_connections = 0;
    bool finished = false;
    int listening_port = 0;
    pool_id_t default_pool_id = 0;

    portmap_service_t pmap;
    ring_loop_t *ringloop = NULL;
    epoll_manager_t *epmgr = NULL;
    cluster_client_t *cli = NULL;
    cli_tool_t *cmd = NULL;
    vitastorkv_dbw_t *db = NULL;
    kv_fs_state_t *kvfs = NULL;
    block_fs_state_t *blockfs = NULL;

    std::vector<XDR*> xdr_pool;

    // inode ID => statistics
    std::map<inode_t, json11::Json> inode_stats;
    // pool ID => statistics
    std::map<pool_id_t, json11::Json> pool_stats;

    ~nfs_proxy_t();

    static json11::Json::object parse_args(int narg, const char *args[]);
    void run(json11::Json cfg);
    void run_server(json11::Json cfg);
    void watch_stats();
    void parse_stats(etcd_kv_t & kv);
    void check_default_pool();
    void do_accept(int listen_fd);
    void daemonize();
    void write_pid();
    void mount_fs();
    void check_already_mounted();
    void check_exit();
};

struct rpc_cur_buffer_t
{
    uint8_t *buf;
    unsigned size;
    unsigned read_pos;
    unsigned parsed_pos;
    int refs;
};

struct rpc_used_buffer_t
{
    unsigned size;
    int refs;
};

struct rpc_free_buffer_t
{
    uint8_t *buf;
    unsigned size;
};

class nfs_client_t
{
public:
    nfs_proxy_t *parent = NULL;
    int nfs_fd;
    int epoll_events = 0;
    int refs = 0;
    bool stopped = false;
    std::set<rpc_service_proc_t> proc_table;

    // Read state
    rpc_cur_buffer_t cur_buffer = { 0 };
    std::map<uint8_t*, rpc_used_buffer_t> used_buffers;
    std::vector<rpc_free_buffer_t> free_buffers;

    iovec read_iov;
    msghdr read_msg = { 0 };

    // Write state
    msghdr write_msg = { 0 };
    std::vector<iovec> send_list, next_send_list;
    std::vector<rpc_op_t*> outbox, next_outbox;

    void select_read_buffer(unsigned wanted_size);
    void submit_read(unsigned wanted_size);
    void handle_read(int result);
    void submit_send();
    void handle_send(int result);
    int handle_rpc_message(void *base_buf, void *msg_buf, uint32_t msg_len);

    bool deref();
    void stop();
};
