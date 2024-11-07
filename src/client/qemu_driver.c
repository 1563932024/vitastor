// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.1 or GNU GPL-2.0+ (see README.md for details)

// QEMU block driver

#ifdef VITASTOR_SOURCE_TREE
#define BUILD_DSO
#define _GNU_SOURCE
#endif
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#if QEMU_VERSION_MAJOR >= 8
#include "block/block-io.h"
#endif
#include "block/block_int.h"
#include "qapi/error.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qerror.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/option.h"

#if QEMU_VERSION_MAJOR >= 3
#include "qemu/units.h"
#include "block/qdict.h"
#include "qemu/cutils.h"
#elif QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR >= 10
#include "qemu/cutils.h"
#include "qapi/qmp/qstring.h"
#include "qapi/qmp/qjson.h"
#else
#include "qapi/qmp/qint.h"
#define qdict_put_int(options, name, num_val) qdict_put_obj(options, name, QOBJECT(qint_from_int(num_val)))
#define qdict_put_str(options, name, value) qdict_put_obj(options, name, QOBJECT(qstring_from_str(value)))
#define qobject_unref QDECREF
#endif
#if QEMU_VERSION_MAJOR == 4 && QEMU_VERSION_MINOR >= 2 || QEMU_VERSION_MAJOR > 4
#include "sysemu/replay.h"
#else
#include "sysemu/sysemu.h"
#endif

#include "vitastor_c.h"

#ifdef VITASTOR_SOURCE_TREE
void qemu_module_dummy(void)
{
}

void DSO_STAMP_FUN(void)
{
}
#endif

typedef struct VitastorFdData VitastorFdData;

typedef struct VitastorClient
{
    void *proxy;
    int uring_eventfd;

    void *watch;
    char *config_path;
    char *etcd_host;
    char *etcd_prefix;
    char *image;
    int skip_parents;
    uint64_t inode;
    uint64_t pool;
    uint64_t size;
    long readonly;
    int use_rdma;
    char *rdma_device;
    int rdma_port_num;
    int rdma_gid_index;
    int rdma_mtu;
    QemuMutex mutex;
    AioContext *ctx;
    VitastorFdData **fds;
    int fd_count, fd_alloc;
    int bh_uring_scheduled;

    uint64_t last_bitmap_inode, last_bitmap_offset, last_bitmap_len;
    uint32_t last_bitmap_granularity;
    uint8_t *last_bitmap;
} VitastorClient;

typedef struct VitastorFdData
{
    VitastorClient *cli;
    int fd;
    IOHandler *fd_read, *fd_write;
    void *opaque;
} VitastorFdData;

typedef struct VitastorRPC
{
    BlockDriverState *bs;
    Coroutine *co;
    QEMUIOVector *iov;
    long ret;
    int complete;
    uint64_t inode, offset, len;
    uint32_t bitmap_granularity;
    uint8_t *bitmap;
#if QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR < 8
    QEMUBH *bh;
#endif
} VitastorRPC;

#if QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR < 8
typedef struct VitastorBH
{
    VitastorClient *cli;
    QEMUBH *bh;
} VitastorBH;
#endif

static void vitastor_co_init_task(BlockDriverState *bs, VitastorRPC *task);
static void vitastor_co_generic_cb(void *opaque, long retval);
static void vitastor_co_read_cb(void *opaque, long retval, uint64_t version);
static void vitastor_close(BlockDriverState *bs);

static char *qemu_vitastor_next_tok(char *src, char delim, char **p)
{
    char *end;
    *p = NULL;
    for (end = src; *end; ++end)
    {
        if (*end == delim)
            break;
        if (*end == '\\' && end[1] != '\0')
            end++;
    }
    if (*end == delim)
    {
        *p = end + 1;
        *end = '\0';
    }
    return src;
}

static void qemu_vitastor_unescape(char *src)
{
    char *p;
    for (p = src; *src; ++src, ++p)
    {
        if (*src == '\\' && src[1] != '\0')
            src++;
        *p = *src;
    }
    *p = '\0';
}

// vitastor[:key=value]*
// vitastor[:etcd_host=127.0.0.1]:inode=1:pool=1[:rdma_gid_index=3]
// vitastor:config_path=/etc/vitastor/vitastor.conf:image=testimg
static void vitastor_parse_filename(const char *filename, QDict *options, Error **errp)
{
    const char *start;
    char *p, *buf;

    if (!strstart(filename, "vitastor:", &start))
    {
        error_setg(errp, "File name must start with 'vitastor:'");
        return;
    }

    buf = g_strdup(start);
    p = buf;

    // The following are all key/value pairs
    while (p)
    {
        int i;
        char *name, *value;
        name = qemu_vitastor_next_tok(p, '=', &p);
        if (!p)
        {
            error_setg(errp, "conf option %s has no value", name);
            break;
        }
        for (i = 0; i < strlen(name); i++)
            if (name[i] == '_')
                name[i] = '-';
        qemu_vitastor_unescape(name);
        value = qemu_vitastor_next_tok(p, ':', &p);
        qemu_vitastor_unescape(value);
        if (!strcmp(name, "inode") ||
            !strcmp(name, "pool") ||
            !strcmp(name, "size") ||
            !strcmp(name, "skip-parents") ||
            !strcmp(name, "use-rdma") ||
            !strcmp(name, "rdma-port_num") ||
            !strcmp(name, "rdma-gid-index") ||
            !strcmp(name, "rdma-mtu"))
        {
#if QEMU_VERSION_MAJOR < 8 || QEMU_VERSION_MAJOR == 8 && QEMU_VERSION_MINOR < 1
            unsigned long long num_val;
            if (parse_uint_full(value, &num_val, 0))
#else
            uint64_t num_val;
            if (parse_uint_full(value, 0, &num_val))
#endif
            {
                error_setg(errp, "Illegal %s: %s", name, value);
                goto out;
            }
            qdict_put_int(options, name, num_val);
        }
        else
        {
            qdict_put_str(options, name, value);
        }
    }
    if (!qdict_get_try_str(options, "image"))
    {
        if (!qdict_get_try_int(options, "inode", 0))
        {
            error_setg(errp, "one of image (name) and inode (number) must be specified");
            goto out;
        }
        if (!(qdict_get_try_int(options, "inode", 0) >> (64-POOL_ID_BITS)) &&
            !qdict_get_try_int(options, "pool", 0))
        {
            error_setg(errp, "pool number must be specified or included in the inode number");
            goto out;
        }
        if (!qdict_get_try_int(options, "size", 0))
        {
            error_setg(errp, "size must be specified when inode number is used instead of image name");
            goto out;
        }
    }

out:
    g_free(buf);
    return;
}

#if defined VITASTOR_C_API_VERSION && VITASTOR_C_API_VERSION >= 2
static void vitastor_uring_handler(void *opaque)
{
    VitastorClient *client = (VitastorClient*)opaque;
    qemu_mutex_lock(&client->mutex);
    client->bh_uring_scheduled = 0;
    vitastor_c_uring_handle_events(client->proxy);
    qemu_mutex_unlock(&client->mutex);
}

#if QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR < 8
static void vitastor_bh_uring_handler(void *opaque)
{
    VitastorBH *vbh = opaque;
    vitastor_bh_handler(vbh->cli);
    qemu_bh_delete(vbh->bh);
    free(vbh);
}
#endif

static void vitastor_schedule_uring_handler(VitastorClient *client)
{
    void *opaque = client;
    if (client->uring_eventfd >= 0 && !client->bh_uring_scheduled)
    {
        client->bh_uring_scheduled = 1;
#if QEMU_VERSION_MAJOR > 4 || QEMU_VERSION_MAJOR == 4 && QEMU_VERSION_MINOR >= 2
        replay_bh_schedule_oneshot_event(client->ctx, vitastor_uring_handler, opaque);
#elif QEMU_VERSION_MAJOR >= 3 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR >= 8
        aio_bh_schedule_oneshot(client->ctx, vitastor_uring_handler, opaque);
#else
        VitastorBH *vbh = (VitastorBH*)malloc(sizeof(VitastorBH));
        vbh->cli = client;
#if QEMU_VERSION_MAJOR >= 2
        vbh->bh = aio_bh_new(bdrv_get_aio_context(task->bs), vitastor_bh_uring_handler, vbh);
#else
        vbh->bh = qemu_bh_new(vitastor_bh_uring_handler, vbh);
#endif
        qemu_bh_schedule(vbh->bh);
#endif
    }
}
#else
static void vitastor_schedule_uring_handler(VitastorClient *client)
{
}
#endif

static void coroutine_fn vitastor_co_get_metadata(VitastorRPC *task)
{
    BlockDriverState *bs = task->bs;
    VitastorClient *client = bs->opaque;
    task->co = qemu_coroutine_self();

    qemu_mutex_lock(&client->mutex);
    vitastor_c_watch_inode(client->proxy, client->image, vitastor_co_generic_cb, task);
    vitastor_schedule_uring_handler(client);
    qemu_mutex_unlock(&client->mutex);

    while (!task->complete)
    {
        qemu_coroutine_yield();
    }
}

static void vitastor_aio_fd_read(void *fddv)
{
    VitastorFdData *fdd = (VitastorFdData*)fddv;
    qemu_mutex_lock(&fdd->cli->mutex);
    fdd->fd_read(fdd->opaque);
    vitastor_schedule_uring_handler(fdd->cli);
    qemu_mutex_unlock(&fdd->cli->mutex);
}

static void vitastor_aio_fd_write(void *fddv)
{
    VitastorFdData *fdd = (VitastorFdData*)fddv;
    qemu_mutex_lock(&fdd->cli->mutex);
    fdd->fd_write(fdd->opaque);
    vitastor_schedule_uring_handler(fdd->cli);
    qemu_mutex_unlock(&fdd->cli->mutex);
}

static void universal_aio_set_fd_handler(AioContext *ctx, int fd, IOHandler *fd_read, IOHandler *fd_write, void *opaque)
{
    aio_set_fd_handler(ctx, fd,
#if QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR >= 5 || QEMU_VERSION_MAJOR >= 3 && (QEMU_VERSION_MAJOR < 8 || QEMU_VERSION_MAJOR == 8 && QEMU_VERSION_MINOR < 1)
        0 /*is_external*/,
#endif
        fd_read,
        fd_write,
#if QEMU_VERSION_MAJOR == 1 && QEMU_VERSION_MINOR <= 6 || QEMU_VERSION_MAJOR < 1
        NULL /*io_flush*/,
#endif
#if QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR >= 9 || QEMU_VERSION_MAJOR >= 3
        NULL /*io_poll*/,
#endif
#if QEMU_VERSION_MAJOR >= 7
        NULL /*io_poll_ready*/,
#endif
        opaque);
}

static void vitastor_aio_set_fd_handler(void *vcli, int fd, int unused1, IOHandler *fd_read, IOHandler *fd_write, void *unused2, void *opaque)
{
    VitastorClient *client = (VitastorClient*)vcli;
    VitastorFdData *fdd = NULL;
    int i;
    for (i = 0; i < client->fd_count; i++)
    {
        if (client->fds[i]->fd == fd)
        {
            if (fd_read || fd_write)
            {
                fdd = client->fds[i];
                fdd->opaque = opaque;
                fdd->fd_read = fd_read;
                fdd->fd_write = fd_write;
            }
            else
            {
                for (int j = i+1; j < client->fd_count; j++)
                    client->fds[j-1] = client->fds[j];
                client->fd_count--;
            }
            break;
        }
    }
    if ((fd_read || fd_write) && !fdd)
    {
        fdd = (VitastorFdData*)malloc(sizeof(VitastorFdData));
        fdd->cli = client;
        fdd->fd = fd;
        fdd->fd_read = fd_read;
        fdd->fd_write = fd_write;
        fdd->opaque = opaque;
        if (client->fd_count >= client->fd_alloc)
        {
            client->fd_alloc = client->fd_alloc*2;
            if (client->fd_alloc < 16)
                client->fd_alloc = 16;
            client->fds = (VitastorFdData**)realloc(client->fds, sizeof(VitastorFdData*) * client->fd_alloc);
        }
        client->fds[client->fd_count++] = fdd;
    }
    universal_aio_set_fd_handler(
        client->ctx, fd, fd_read ? vitastor_aio_fd_read : NULL, fd_write ? vitastor_aio_fd_write : NULL, fdd
    );
}

#if defined VITASTOR_C_API_VERSION && VITASTOR_C_API_VERSION >= 2
typedef struct str_array
{
    const char **items;
    int len, alloc;
} str_array;

static void strarray_push(str_array *a, const char *str)
{
    if (a->len >= a->alloc)
    {
        a->alloc = !a->alloc ? 4 : 2*a->alloc;
        a->items = (const char**)realloc(a->items, a->alloc*sizeof(char*));
        if (!a->items)
        {
            fprintf(stderr, "bad alloc\n");
            abort();
        }
    }
    a->items[a->len++] = str;
}

static void strarray_push_kv(str_array *a, const char *key, const char *value)
{
    if (key && value)
    {
        strarray_push(a, key);
        strarray_push(a, value);
    }
}

static void strarray_free(str_array *a)
{
    free(a->items);
    a->items = NULL;
    a->len = a->alloc = 0;
}
#endif

static int vitastor_file_open(BlockDriverState *bs, QDict *options, int flags, Error **errp)
{
    VitastorRPC task;
    VitastorClient *client = bs->opaque;
    void *image = NULL;
    int64_t ret = 0;
    qemu_mutex_init(&client->mutex);
    client->config_path = g_strdup(qdict_get_try_str(options, "config-path"));
    // FIXME: Rename to etcd_address
    client->etcd_host = g_strdup(qdict_get_try_str(options, "etcd-host"));
    client->etcd_prefix = g_strdup(qdict_get_try_str(options, "etcd-prefix"));
    client->skip_parents = qdict_get_try_int(options, "skip-parents", 0);
    client->use_rdma = qdict_get_try_int(options, "use-rdma", -1);
    client->rdma_device = g_strdup(qdict_get_try_str(options, "rdma-device"));
    client->rdma_port_num = qdict_get_try_int(options, "rdma-port-num", 0);
    client->rdma_gid_index = qdict_get_try_int(options, "rdma-gid-index", 0);
    client->rdma_mtu = qdict_get_try_int(options, "rdma-mtu", 0);
    client->ctx = bdrv_get_aio_context(bs);
#if defined VITASTOR_C_API_VERSION && VITASTOR_C_API_VERSION >= 2
    str_array opt = {};
    strarray_push_kv(&opt, "config_path", qdict_get_try_str(options, "config-path"));
    strarray_push_kv(&opt, "etcd_address", qdict_get_try_str(options, "etcd-host"));
    strarray_push_kv(&opt, "etcd_prefix", qdict_get_try_str(options, "etcd-prefix"));
    strarray_push_kv(&opt, "use_rdma", qdict_get_try_str(options, "use-rdma"));
    strarray_push_kv(&opt, "rdma_device", qdict_get_try_str(options, "rdma-device"));
    strarray_push_kv(&opt, "rdma_port_num", qdict_get_try_str(options, "rdma-port-num"));
    strarray_push_kv(&opt, "rdma_gid_index", qdict_get_try_str(options, "rdma-gid-index"));
    strarray_push_kv(&opt, "rdma_mtu", qdict_get_try_str(options, "rdma-mtu"));
    strarray_push_kv(&opt, "client_writeback_allowed", (flags & BDRV_O_NOCACHE) ? "0" : "1");
    client->proxy = vitastor_c_create_uring_json(opt.items, opt.len);
    strarray_free(&opt);
    if (client->proxy)
    {
        client->uring_eventfd = vitastor_c_uring_register_eventfd(client->proxy);
        if (client->uring_eventfd < 0)
        {
            fprintf(stderr, "vitastor: failed to create io_uring eventfd: %s\n", strerror(errno));
            error_setg(errp, "failed to create io_uring eventfd");
            vitastor_close(bs);
            return -1;
        }
        universal_aio_set_fd_handler(client->ctx, client->uring_eventfd, vitastor_uring_handler, NULL, client);
    }
    else
    {
        // Writeback cache is unusable without io_uring because the client can't correctly flush on exit
        fprintf(stderr, "vitastor: failed to create io_uring: %s - I/O will be slower%s\n",
            strerror(errno), (flags & BDRV_O_NOCACHE ? "" : " and writeback cache will be disabled"));
#endif
        client->uring_eventfd = -1;
        client->proxy = vitastor_c_create_qemu(
            vitastor_aio_set_fd_handler, client, client->config_path, client->etcd_host, client->etcd_prefix,
            client->use_rdma, client->rdma_device, client->rdma_port_num, client->rdma_gid_index, client->rdma_mtu, 0
        );
#if defined VITASTOR_C_API_VERSION && VITASTOR_C_API_VERSION >= 2
    }
#endif
    image = client->image = g_strdup(qdict_get_try_str(options, "image"));
    client->readonly = (flags & BDRV_O_RDWR) ? 1 : 0;
    // Get image metadata (size and readonly flag) or just wait until the client is ready
    if (!image)
        client->image = (char*)"x";
    task.complete = 0;
    task.bs = bs;
    if (qemu_in_coroutine())
    {
        vitastor_co_get_metadata(&task);
    }
    else
    {
#if QEMU_VERSION_MAJOR >= 8
        aio_co_enter(bdrv_get_aio_context(bs), qemu_coroutine_create((void(*)(void*))vitastor_co_get_metadata, &task));
#elif QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR >= 9 || QEMU_VERSION_MAJOR >= 3
        bdrv_coroutine_enter(bs, qemu_coroutine_create((void(*)(void*))vitastor_co_get_metadata, &task));
#else
        qemu_coroutine_enter(qemu_coroutine_create((void(*)(void*))vitastor_co_get_metadata, &task));
#endif
        BDRV_POLL_WHILE(bs, !task.complete);
    }
    client->image = image;
    if (client->image)
    {
        client->watch = (void*)task.ret;
        client->readonly = client->readonly || vitastor_c_inode_get_readonly(client->watch);
        client->size = vitastor_c_inode_get_size(client->watch);
        if (!vitastor_c_inode_get_num(client->watch))
        {
            error_setg(errp, "image does not exist");
            vitastor_close(bs);
            return -1;
        }
        if (!client->size)
        {
            client->size = qdict_get_try_int(options, "size", 0);
        }
    }
    else
    {
        client->watch = NULL;
        client->inode = qdict_get_try_int(options, "inode", 0);
        client->pool = qdict_get_try_int(options, "pool", 0);
        if (client->pool)
        {
            client->inode = (client->inode & (((uint64_t)1 << (64-POOL_ID_BITS)) - 1)) | (client->pool << (64-POOL_ID_BITS));
        }
        client->size = qdict_get_try_int(options, "size", 0);
        vitastor_c_close_watch(client->proxy, (void*)task.ret);
    }
    if (!client->size)
    {
        error_setg(errp, "image size not specified");
        vitastor_close(bs);
        return -1;
    }
    bs->total_sectors = client->size / BDRV_SECTOR_SIZE;
#if QEMU_VERSION_MAJOR > 5 || QEMU_VERSION_MAJOR == 5 && QEMU_VERSION_MINOR >= 1
    /* When extending regular files, we get zeros from the OS */
    bs->supported_truncate_flags = BDRV_REQ_ZERO_WRITE;
#endif
    //client->aio_context = bdrv_get_aio_context(bs);
    qdict_del(options, "use-rdma");
    qdict_del(options, "rdma-mtu");
    qdict_del(options, "rdma-gid-index");
    qdict_del(options, "rdma-port-num");
    qdict_del(options, "rdma-device");
    qdict_del(options, "config-path");
    qdict_del(options, "etcd-host");
    qdict_del(options, "etcd-prefix");
    qdict_del(options, "image");
    qdict_del(options, "inode");
    qdict_del(options, "pool");
    qdict_del(options, "size");
    qdict_del(options, "skip-parents");
    return ret;
}

static void vitastor_close(BlockDriverState *bs)
{
    VitastorClient *client = bs->opaque;
    vitastor_c_destroy(client->proxy);
    if (client->fds)
    {
        free(client->fds);
        client->fds = NULL;
        client->fd_alloc = client->fd_count = 0;
    }
    qemu_mutex_destroy(&client->mutex);
    if (client->config_path)
        g_free(client->config_path);
    if (client->etcd_host)
        g_free(client->etcd_host);
    if (client->etcd_prefix)
        g_free(client->etcd_prefix);
    if (client->image)
        g_free(client->image);
    free(client->last_bitmap);
    client->last_bitmap = NULL;
}

#if QEMU_VERSION_MAJOR >= 3 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR > 2
static int vitastor_probe_blocksizes(BlockDriverState *bs, BlockSizes *bsz)
{
    bsz->phys = 4096;
    bsz->log = 512;
    return 0;
}
#endif

#if QEMU_VERSION_MAJOR >= 3 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR >= 12
static int coroutine_fn vitastor_co_create_opts(
#if QEMU_VERSION_MAJOR >= 4
    BlockDriver *drv,
#endif
    const char *url, QemuOpts *opts, Error **errp)
{
    QDict *options;
    int ret;

    options = qdict_new();
    vitastor_parse_filename(url, options, errp);
    if (*errp)
    {
        ret = -1;
        goto out;
    }

    // inodes don't require creation in Vitastor. FIXME: They will when there will be some metadata

    ret = 0;
out:
    qobject_unref(options);
    return ret;
}
#endif

#if QEMU_VERSION_MAJOR >= 3
static int coroutine_fn vitastor_co_truncate(BlockDriverState *bs, int64_t offset,
#if QEMU_VERSION_MAJOR >= 4
    bool exact,
#endif
    PreallocMode prealloc,
#if QEMU_VERSION_MAJOR >= 5 && QEMU_VERSION_MINOR >= 1 || QEMU_VERSION_MAJOR > 5 || defined RHEL_BDRV_CO_TRUNCATE_FLAGS
    BdrvRequestFlags flags,
#endif
    Error **errp)
{
    VitastorClient *client = bs->opaque;

    if (prealloc != PREALLOC_MODE_OFF)
    {
        error_setg(errp, "Unsupported preallocation mode '%s'", PreallocMode_str(prealloc));
        return -ENOTSUP;
    }

    // TODO: Resize inode to <offset> bytes
#if QEMU_VERSION_MAJOR >= 4
    client->size = exact || client->size < offset ? offset : client->size;
#else
    client->size = offset;
#endif

    return 0;
}
#endif

static int vitastor_get_info(BlockDriverState *bs, BlockDriverInfo *bdi)
{
    bdi->cluster_size = 4096;
    return 0;
}

static int64_t vitastor_getlength(BlockDriverState *bs)
{
    VitastorClient *client = bs->opaque;
    return client->size;
}

#if QEMU_VERSION_MAJOR >= 3 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR > 0
static void vitastor_refresh_limits(BlockDriverState *bs, Error **errp)
#else
static int vitastor_refresh_limits(BlockDriverState *bs)
#endif
{
    bs->bl.request_alignment = 4096;
#if QEMU_VERSION_MAJOR >= 3 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR > 3
    bs->bl.min_mem_alignment = 4096;
#endif
    bs->bl.opt_mem_alignment = 4096;
#if QEMU_VERSION_MAJOR < 2 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR == 0
    return 0;
#endif
}

//static int64_t vitastor_get_allocated_file_size(BlockDriverState *bs)
//{
//    return 0;
//}

static void vitastor_co_init_task(BlockDriverState *bs, VitastorRPC *task)
{
    *task = (VitastorRPC) {
        .co     = qemu_coroutine_self(),
        .bs     = bs,
    };
}

static void vitastor_co_generic_bh_cb(void *opaque)
{
    VitastorRPC *task = opaque;
    task->complete = 1;
    if (qemu_coroutine_self() != task->co)
    {
#if QEMU_VERSION_MAJOR >= 3 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR > 8
        aio_co_wake(task->co);
#else
#if QEMU_VERSION_MAJOR == 2
        qemu_bh_delete(task->bh);
#endif
        qemu_coroutine_enter(task->co, NULL);
        qemu_aio_release(task);
#endif
    }
}

static void vitastor_co_generic_cb(void *opaque, long retval)
{
    VitastorRPC *task = opaque;
    task->ret = retval;
#if QEMU_VERSION_MAJOR > 4 || QEMU_VERSION_MAJOR == 4 && QEMU_VERSION_MINOR >= 2
    replay_bh_schedule_oneshot_event(bdrv_get_aio_context(task->bs), vitastor_co_generic_bh_cb, opaque);
#elif QEMU_VERSION_MAJOR >= 3 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR >= 8
    aio_bh_schedule_oneshot(bdrv_get_aio_context(task->bs), vitastor_co_generic_bh_cb, opaque);
#elif QEMU_VERSION_MAJOR >= 2
    task->bh = aio_bh_new(bdrv_get_aio_context(task->bs), vitastor_co_generic_bh_cb, opaque);
    qemu_bh_schedule(task->bh);
#else
    task->bh = qemu_bh_new(vitastor_co_generic_bh_cb, opaque);
    qemu_bh_schedule(task->bh);
#endif
}

static void vitastor_co_read_cb(void *opaque, long retval, uint64_t version)
{
    vitastor_co_generic_cb(opaque, retval);
}

static int coroutine_fn vitastor_co_preadv(BlockDriverState *bs,
#if QEMU_VERSION_MAJOR >= 7 || QEMU_VERSION_MAJOR == 6 && QEMU_VERSION_MINOR >= 2
    int64_t offset, int64_t bytes, QEMUIOVector *iov, BdrvRequestFlags flags
#else
    uint64_t offset, uint64_t bytes, QEMUIOVector *iov, int flags
#endif
)
{
    VitastorClient *client = bs->opaque;
    VitastorRPC task;
    vitastor_co_init_task(bs, &task);
    task.iov = iov;

    uint64_t inode = client->watch ? vitastor_c_inode_get_num(client->watch) : client->inode;
    qemu_mutex_lock(&client->mutex);
    vitastor_c_read(client->proxy, inode, offset, bytes, iov->iov, iov->niov, vitastor_co_read_cb, &task);
    vitastor_schedule_uring_handler(client);
    qemu_mutex_unlock(&client->mutex);

    while (!task.complete)
    {
        qemu_coroutine_yield();
    }

    return task.ret;
}

static int coroutine_fn vitastor_co_pwritev(BlockDriverState *bs,
#if QEMU_VERSION_MAJOR >= 7 || QEMU_VERSION_MAJOR == 6 && QEMU_VERSION_MINOR >= 2
    int64_t offset, int64_t bytes, QEMUIOVector *iov, BdrvRequestFlags flags
#else
    uint64_t offset, uint64_t bytes, QEMUIOVector *iov, int flags
#endif
)
{
    VitastorClient *client = bs->opaque;
    VitastorRPC task;
    vitastor_co_init_task(bs, &task);
    task.iov = iov;

    if (client->last_bitmap)
    {
        // Invalidate last bitmap on write
        free(client->last_bitmap);
        client->last_bitmap = NULL;
    }

    uint64_t inode = client->watch ? vitastor_c_inode_get_num(client->watch) : client->inode;
    qemu_mutex_lock(&client->mutex);
    vitastor_c_write(client->proxy, inode, offset, bytes, 0, iov->iov, iov->niov, vitastor_co_generic_cb, &task);
    vitastor_schedule_uring_handler(client);
    qemu_mutex_unlock(&client->mutex);

    while (!task.complete)
    {
        qemu_coroutine_yield();
    }

    return task.ret;
}

#if defined VITASTOR_C_API_VERSION && VITASTOR_C_API_VERSION >= 1
#if QEMU_VERSION_MAJOR >= 2 || QEMU_VERSION_MAJOR == 1 && QEMU_VERSION_MINOR >= 7
static void vitastor_co_read_bitmap_cb(void *opaque, long retval, uint8_t *bitmap)
{
    VitastorRPC *task = opaque;
    VitastorClient *client = task->bs->opaque;
    task->ret = retval;
    if (retval >= 0)
    {
        task->bitmap = bitmap;
        if (client->last_bitmap_inode == task->inode &&
            client->last_bitmap_offset == task->offset &&
            client->last_bitmap_len == task->len)
        {
            free(client->last_bitmap);
            client->last_bitmap = bitmap;
        }
    }
#if QEMU_VERSION_MAJOR > 4 || QEMU_VERSION_MAJOR == 4 && QEMU_VERSION_MINOR >= 2
    replay_bh_schedule_oneshot_event(bdrv_get_aio_context(task->bs), vitastor_co_generic_bh_cb, opaque);
#elif QEMU_VERSION_MAJOR >= 3 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR >= 8
    aio_bh_schedule_oneshot(bdrv_get_aio_context(task->bs), vitastor_co_generic_bh_cb, opaque);
#elif QEMU_VERSION_MAJOR >= 2
    task->bh = aio_bh_new(bdrv_get_aio_context(task->bs), vitastor_co_generic_bh_cb, opaque);
    qemu_bh_schedule(task->bh);
#else
    task->bh = qemu_bh_new(vitastor_co_generic_bh_cb, opaque);
    qemu_bh_schedule(task->bh);
#endif
}

static int coroutine_fn vitastor_co_block_status(
    BlockDriverState *bs, bool want_zero, int64_t offset, int64_t bytes,
    int64_t *pnum, int64_t *map, BlockDriverState **file)
{
    // Allocated => return BDRV_BLOCK_DATA|BDRV_BLOCK_OFFSET_VALID
    // Not allocated => return 0
    // Error => return -errno
    // Set pnum to length of the extent, `*map` = `offset`, `*file` = `bs`
    VitastorRPC task;
    VitastorClient *client = bs->opaque;
    uint64_t inode = client->watch ? vitastor_c_inode_get_num(client->watch) : client->inode;
    uint8_t bit = 0;
    if (client->last_bitmap && client->last_bitmap_inode == inode &&
        client->last_bitmap_offset <= offset &&
        client->last_bitmap_offset+client->last_bitmap_len >= (want_zero ? offset+1 : offset+bytes))
    {
        // Use the previously read bitmap
        task.bitmap_granularity = client->last_bitmap_granularity;
        task.offset = client->last_bitmap_offset;
        task.len = client->last_bitmap_len;
        task.bitmap = client->last_bitmap;
    }
    else
    {
        // Read bitmap from this position, rounding to full inode PG blocks
        uint32_t block_size = vitastor_c_inode_get_block_size(client->proxy, inode);
        if (!block_size)
            return -EAGAIN;
        // Init coroutine
        vitastor_co_init_task(bs, &task);
        free(client->last_bitmap);
        task.inode = client->last_bitmap_inode = inode;
        task.bitmap_granularity = client->last_bitmap_granularity = vitastor_c_inode_get_bitmap_granularity(client->proxy, inode);
        task.offset = client->last_bitmap_offset = offset / block_size * block_size;
        task.len = client->last_bitmap_len = (offset+bytes+block_size-1) / block_size * block_size - task.offset;
        task.bitmap = client->last_bitmap = NULL;
        qemu_mutex_lock(&client->mutex);
        vitastor_c_read_bitmap(client->proxy, task.inode, task.offset, task.len, !client->skip_parents, vitastor_co_read_bitmap_cb, &task);
        vitastor_schedule_uring_handler(client);
        qemu_mutex_unlock(&client->mutex);
        while (!task.complete)
        {
            qemu_coroutine_yield();
        }
        if (task.ret < 0)
        {
            // Error
            return task.ret;
        }
    }
    if (want_zero)
    {
        // Get precise mapping with all holes
        uint64_t bmp_pos = (offset-task.offset) / task.bitmap_granularity;
        uint64_t bmp_len = task.len / task.bitmap_granularity;
        uint64_t bmp_end = bmp_pos+1;
        bit = (task.bitmap[bmp_pos >> 3] >> (bmp_pos & 0x7)) & 1;
        while (bmp_end < bmp_len && ((task.bitmap[bmp_end >> 3] >> (bmp_end & 0x7)) & 1) == bit)
        {
            bmp_end++;
        }
        *pnum = (bmp_end-bmp_pos) * task.bitmap_granularity;
    }
    else
    {
        // Get larger allocated extents, possibly with false positives
        uint64_t bmp_pos = (offset-task.offset) / task.bitmap_granularity;
        uint64_t bmp_end = (offset+bytes-task.offset) / task.bitmap_granularity - bmp_pos;
        while (bmp_pos < bmp_end)
        {
            if (!(bmp_pos & 7) && bmp_end >= bmp_pos+8)
            {
                bit = bit || task.bitmap[bmp_pos >> 3];
                bmp_pos += 8;
            }
            else
            {
                bit = bit || ((task.bitmap[bmp_pos >> 3] >> (bmp_pos & 0x7)) & 1);
                bmp_pos++;
            }
        }
        *pnum = bytes;
    }
    if (bit)
    {
        *map = offset;
        *file = bs;
    }
    return (bit ? (BDRV_BLOCK_DATA|BDRV_BLOCK_OFFSET_VALID) : 0);
}
#endif
#if QEMU_VERSION_MAJOR == 1 && QEMU_VERSION_MINOR >= 7 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR < 12
// QEMU 1.7-2.11
static int64_t coroutine_fn vitastor_co_get_block_status(BlockDriverState *bs,
    int64_t sector_num, int nb_sectors, int *pnum, BlockDriverState **file)
{
    int64_t map = 0;
    int64_t pnumbytes = 0;
    int r = vitastor_co_block_status(bs, 1, sector_num*BDRV_SECTOR_SIZE, nb_sectors*BDRV_SECTOR_SIZE, &pnumbytes, &map, &file);
    *pnum = pnumbytes/BDRV_SECTOR_SIZE;
    return r;
}
#endif
#endif

#if !( QEMU_VERSION_MAJOR >= 3 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR >= 7 )
static int coroutine_fn vitastor_co_readv(BlockDriverState *bs, int64_t sector_num, int nb_sectors, QEMUIOVector *iov)
{
    return vitastor_co_preadv(bs, sector_num*BDRV_SECTOR_SIZE, nb_sectors*BDRV_SECTOR_SIZE, iov, 0);
}

static int coroutine_fn vitastor_co_writev(BlockDriverState *bs, int64_t sector_num, int nb_sectors, QEMUIOVector *iov)
{
    return vitastor_co_pwritev(bs, sector_num*BDRV_SECTOR_SIZE, nb_sectors*BDRV_SECTOR_SIZE, iov, 0);
}
#endif

static int coroutine_fn vitastor_co_flush(BlockDriverState *bs)
{
    VitastorClient *client = bs->opaque;
    VitastorRPC task;
    vitastor_co_init_task(bs, &task);

    qemu_mutex_lock(&client->mutex);
    vitastor_c_sync(client->proxy, vitastor_co_generic_cb, &task);
    vitastor_schedule_uring_handler(client);
    qemu_mutex_unlock(&client->mutex);

    while (!task.complete)
    {
        qemu_coroutine_yield();
    }

    return task.ret;
}

#if QEMU_VERSION_MAJOR >= 3 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR > 0
static QemuOptsList vitastor_create_opts = {
    .name = "vitastor-create-opts",
    .head = QTAILQ_HEAD_INITIALIZER(vitastor_create_opts.head),
    .desc = {
        {
            .name = BLOCK_OPT_SIZE,
            .type = QEMU_OPT_SIZE,
            .help = "Virtual disk size"
        },
        { /* end of list */ }
    }
};
#else
static QEMUOptionParameter vitastor_create_opts[] = {
    {
        .name = BLOCK_OPT_SIZE,
        .type = OPT_SIZE,
        .help = "Virtual disk size"
    },
    { NULL }
};
#endif

#if QEMU_VERSION_MAJOR >= 4
static const char *vitastor_strong_runtime_opts[] = {
    "inode",
    "pool",
    "config-path",
    "etcd-host",
    "etcd-prefix",

    NULL
};
#endif

static BlockDriver bdrv_vitastor = {
    .format_name                    = "vitastor",
    .protocol_name                  = "vitastor",

    .instance_size                  = sizeof(VitastorClient),
    .bdrv_parse_filename            = vitastor_parse_filename,

    .bdrv_has_zero_init             = bdrv_has_zero_init_1,
#if QEMU_VERSION_MAJOR >= 8
    .bdrv_co_get_info               = vitastor_get_info,
    .bdrv_co_getlength              = vitastor_getlength,
#else
    .bdrv_get_info                  = vitastor_get_info,
    .bdrv_getlength                 = vitastor_getlength,
#endif
#if QEMU_VERSION_MAJOR >= 3 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR > 2
    .bdrv_probe_blocksizes          = vitastor_probe_blocksizes,
#endif
    .bdrv_refresh_limits            = vitastor_refresh_limits,

    // FIXME: Implement it along with per-inode statistics
    //.bdrv_get_allocated_file_size   = vitastor_get_allocated_file_size,

#if QEMU_VERSION_MAJOR > 9 || QEMU_VERSION_MAJOR == 9 && QEMU_VERSION_MINOR > 0
    .bdrv_open                      = vitastor_file_open,
#else
    .bdrv_file_open                 = vitastor_file_open,
#endif
    .bdrv_close                     = vitastor_close,

    // Option list for the create operation
#if QEMU_VERSION_MAJOR >= 3 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR > 0
    .create_opts                    = &vitastor_create_opts,
#else
    .create_options                 = vitastor_create_opts,
#endif

    // For qmp_blockdev_create(), used by the qemu monitor / QAPI
    // Requires patching QAPI IDL, thus unimplemented
    //.bdrv_co_create                 = vitastor_co_create,

#if QEMU_VERSION_MAJOR >= 3 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR >= 12
    // For bdrv_create(), used by qemu-img
    .bdrv_co_create_opts            = vitastor_co_create_opts,
#endif

#if QEMU_VERSION_MAJOR >= 3
    .bdrv_co_truncate               = vitastor_co_truncate,
#endif

#if defined VITASTOR_C_API_VERSION && VITASTOR_C_API_VERSION >= 1
#if QEMU_VERSION_MAJOR >= 3 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR >= 12
    // For snapshot export
    .bdrv_co_block_status           = vitastor_co_block_status,
#elif QEMU_VERSION_MAJOR == 1 && QEMU_VERSION_MINOR >= 7 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR < 12
    .bdrv_co_get_block_status       = vitastor_co_get_block_status,
#endif
#endif

#if QEMU_VERSION_MAJOR >= 3 || QEMU_VERSION_MAJOR == 2 && QEMU_VERSION_MINOR >= 7
    .bdrv_co_preadv                 = vitastor_co_preadv,
    .bdrv_co_pwritev                = vitastor_co_pwritev,
#else
    .bdrv_co_readv                  = vitastor_co_readv,
    .bdrv_co_writev                 = vitastor_co_writev,
#endif

    .bdrv_co_flush_to_disk          = vitastor_co_flush,

#if QEMU_VERSION_MAJOR >= 4
    .strong_runtime_opts            = vitastor_strong_runtime_opts,
#endif
};

static void vitastor_block_init(void)
{
    bdrv_register(&bdrv_vitastor);
}

block_init(vitastor_block_init);
