// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.1 or GNU GPL-2.0+ (see README.md for details)

// FIO engine to test cluster I/O
//
// Random write:
//
// fio -thread -ioengine=./libfio_cluster.so -name=test -bs=4k -direct=1 -fsync=16 -iodepth=16 -rw=randwrite \
//     -etcd=127.0.0.1:2379 [-etcd_prefix=/vitastor] (-image=testimg | -pool=1 -inode=1 -size=1000M)
//
// Linear write:
//
// fio -thread -ioengine=./libfio_cluster.so -name=test -bs=128k -direct=1 -fsync=32 -iodepth=32 -rw=write \
//     -etcd=127.0.0.1:2379 [-etcd_prefix=/vitastor] -image=testimg
//
// Random read (run with -iodepth=32 or -iodepth=1):
//
// fio -thread -ioengine=./libfio_cluster.so -name=test -bs=4k -direct=1 -iodepth=32 -rw=randread \
//     -etcd=127.0.0.1:2379 [-etcd_prefix=/vitastor] -image=testimg

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <vector>
#include <string>

#include "vitastor_c.h"
#include "fio_headers.h"

struct sec_data
{
    vitastor_c *cli = NULL;
    bool epoll_based = false;
    void *watch = NULL;
    bool last_sync = false;
    /* The list of completed io_u structs. */
    std::vector<io_u*> completed;
    uint64_t inflight = 0;
    int mirror_fd = -1;
    bool trace = false;
};

struct sec_options
{
    int __pad;
    char *config_path = NULL;
    char *etcd_host = NULL;
    char *etcd_prefix = NULL;
    char *image = NULL;
    char *mirror_file = NULL;
    uint64_t pool = 0;
    uint64_t inode = 0;
    int cluster_log = 0;
    int trace = 0;
    int use_rdma = 0;
    char *rdma_device = NULL;
    int rdma_port_num = 0;
    int rdma_gid_index = 0;
    int rdma_mtu = 0;
    int no_io_uring = 0;
};

static struct fio_option options[] = {
    {
        .name   = "conf",
        .lname  = "Vitastor config path",
        .type   = FIO_OPT_STR_STORE,
        .off1   = offsetof(struct sec_options, config_path),
        .help   = "Vitastor config path",
        .category = FIO_OPT_C_ENGINE,
        .group  = FIO_OPT_G_FILENAME,
    },
    {
        .name   = "etcd",
        .lname  = "etcd address",
        .type   = FIO_OPT_STR_STORE,
        .off1   = offsetof(struct sec_options, etcd_host),
        .help   = "etcd address in the form HOST:PORT[/PATH]",
        .category = FIO_OPT_C_ENGINE,
        .group  = FIO_OPT_G_FILENAME,
    },
    {
        .name   = "etcd_prefix",
        .lname  = "etcd key prefix",
        .type   = FIO_OPT_STR_STORE,
        .off1   = offsetof(struct sec_options, etcd_prefix),
        .help   = "etcd key prefix, by default /vitastor",
        .category = FIO_OPT_C_ENGINE,
        .group  = FIO_OPT_G_FILENAME,
    },
    {
        .name   = "image",
        .lname  = "Vitastor image name",
        .type   = FIO_OPT_STR_STORE,
        .off1   = offsetof(struct sec_options, image),
        .help   = "Vitastor image name to run tests on",
        .category = FIO_OPT_C_ENGINE,
        .group  = FIO_OPT_G_FILENAME,
    },
    {
        .name   = "pool",
        .lname  = "pool number for the inode",
        .type   = FIO_OPT_INT,
        .off1   = offsetof(struct sec_options, pool),
        .help   = "pool number for the inode to run tests on",
        .category = FIO_OPT_C_ENGINE,
        .group  = FIO_OPT_G_FILENAME,
    },
    {
        .name   = "inode",
        .lname  = "inode to run tests on",
        .type   = FIO_OPT_INT,
        .off1   = offsetof(struct sec_options, inode),
        .help   = "inode number to run tests on",
        .category = FIO_OPT_C_ENGINE,
        .group  = FIO_OPT_G_FILENAME,
    },
    {
        .name   = "cluster_log_level",
        .lname  = "cluster log level",
        .type   = FIO_OPT_INT,
        .off1   = offsetof(struct sec_options, cluster_log),
        .help   = "Set log level for the Vitastor client",
        .def    = "0",
        .category = FIO_OPT_C_ENGINE,
        .group  = FIO_OPT_G_FILENAME,
    },
    {
        .name   = "osd_trace",
        .lname  = "OSD trace",
        .type   = FIO_OPT_BOOL,
        .off1   = offsetof(struct sec_options, trace),
        .help   = "Trace OSD operations",
        .def    = "0",
        .category = FIO_OPT_C_ENGINE,
        .group  = FIO_OPT_G_FILENAME,
    },
    {
        .name   = "mirror_file",
        .lname  = "File name to mirror writes to",
        .type   = FIO_OPT_STR_STORE,
        .off1   = offsetof(struct sec_options, mirror_file),
        .help   = "File name to mirror writes to (for debug purpose)",
        .category = FIO_OPT_C_ENGINE,
        .group  = FIO_OPT_G_FILENAME,
    },
    {
        .name   = "use_rdma",
        .lname  = "Use RDMA",
        .type   = FIO_OPT_BOOL,
        .off1   = offsetof(struct sec_options, use_rdma),
        .help   = "Use RDMA",
        .def    = "-1",
        .category = FIO_OPT_C_ENGINE,
        .group  = FIO_OPT_G_FILENAME,
    },
    {
        .name   = "rdma_device",
        .lname  = "RDMA device name",
        .type   = FIO_OPT_STR_STORE,
        .off1   = offsetof(struct sec_options, rdma_device),
        .help   = "RDMA device name",
        .category = FIO_OPT_C_ENGINE,
        .group  = FIO_OPT_G_FILENAME,
    },
    {
        .name   = "rdma_port_num",
        .lname  = "RDMA port number",
        .type   = FIO_OPT_INT,
        .off1   = offsetof(struct sec_options, rdma_port_num),
        .help   = "RDMA port number",
        .def    = "0",
        .category = FIO_OPT_C_ENGINE,
        .group  = FIO_OPT_G_FILENAME,
    },
    {
        .name   = "rdma_gid_index",
        .lname  = "RDMA gid index",
        .type   = FIO_OPT_INT,
        .off1   = offsetof(struct sec_options, rdma_gid_index),
        .help   = "RDMA gid index",
        .def    = "0",
        .category = FIO_OPT_C_ENGINE,
        .group  = FIO_OPT_G_FILENAME,
    },
    {
        .name   = "rdma_mtu",
        .lname  = "RDMA path MTU",
        .type   = FIO_OPT_INT,
        .off1   = offsetof(struct sec_options, rdma_mtu),
        .help   = "RDMA path MTU",
        .def    = "0",
        .category = FIO_OPT_C_ENGINE,
        .group  = FIO_OPT_G_FILENAME,
    },
    {
        .name   = "no_io_uring",
        .lname  = "Disable io_uring",
        .type   = FIO_OPT_BOOL,
        .off1   = offsetof(struct sec_options, no_io_uring),
        .help   = "Use epoll and plain sendmsg/recvmsg instead of io_uring (slower)",
        .def    = "0",
        .category = FIO_OPT_C_ENGINE,
        .group  = FIO_OPT_G_FILENAME,
    },
    {
        .name = NULL,
    },
};

static void watch_callback(void *opaque, long watch)
{
    struct sec_data *bsd = (struct sec_data*)opaque;
    bsd->watch = (void*)watch;
}

static void opt_push(std::vector<char *> & options, const char *opt, const char *value)
{
    if (value)
    {
        options.push_back(strdup(opt));
        options.push_back(strdup(value));
    }
}

static int sec_setup(struct thread_data *td)
{
    sec_options *o = (sec_options*)td->eo;
    sec_data *bsd;

    bsd = new sec_data;
    if (!bsd)
    {
        td_verror(td, errno, "calloc");
        return 1;
    }
    td->io_ops_data = bsd;

    if (!td->files_index)
    {
        add_file(td, "osd_cluster", 0, 0);
        td->o.nr_files = td->o.nr_files ? : 1;
        td->o.open_files++;
    }

    if (o->mirror_file)
    {
        bsd->mirror_fd = open(o->mirror_file, O_CREAT|O_RDWR, 0666);
        if (bsd->mirror_fd < 0)
        {
            td_verror(td, errno, "open mirror file");
            return 1;
        }
    }

    if (!o->image)
    {
        if (!(o->inode & (((uint64_t)1 << (64-POOL_ID_BITS)) - 1)))
        {
            td_verror(td, EINVAL, "inode number is missing");
            return 1;
        }
        if (o->pool)
        {
            o->inode = (o->inode & (((uint64_t)1 << (64-POOL_ID_BITS)) - 1)) | (o->pool << (64-POOL_ID_BITS));
        }
        if (!(o->inode >> (64-POOL_ID_BITS)))
        {
            td_verror(td, EINVAL, "pool is missing");
            return 1;
        }
    }
    else
    {
        o->inode = 0;
    }
    std::vector<char *> options;
    opt_push(options, "config_path", o->config_path);
    opt_push(options, "etcd_address", o->etcd_host);
    opt_push(options, "etcd_prefix", o->etcd_prefix);
    if (o->use_rdma != -1)
        opt_push(options, "use_rdma", std::to_string(o->use_rdma).c_str());
    opt_push(options, "rdma_device", o->rdma_device);
    if (o->rdma_port_num)
        opt_push(options, "rdma_port_num", std::to_string(o->rdma_port_num).c_str());
    if (o->rdma_gid_index)
        opt_push(options, "rdma_gid_index", std::to_string(o->rdma_gid_index).c_str());
    if (o->rdma_mtu)
        opt_push(options, "rdma_mtu", std::to_string(o->rdma_mtu).c_str());
    if (o->cluster_log)
        opt_push(options, "log_level", std::to_string(o->cluster_log).c_str());
    // allow writeback caching if -direct is not set
    opt_push(options, "client_writeback_allowed", td->o.odirect ? "0" : "1");
    bsd->cli = o->no_io_uring ? NULL : vitastor_c_create_uring_json((const char**)options.data(), options.size());
    bsd->epoll_based = false;
    if (!bsd->cli)
    {
        if (o->no_io_uring)
            fprintf(stderr, "vitastor: io_uring disabled - I/O will be slower\n");
        else
            fprintf(stderr, "vitastor: failed to create io_uring: %s - I/O will be slower\n", strerror(errno));
        bsd->cli = vitastor_c_create_epoll_json((const char**)options.data(), options.size());
        bsd->epoll_based = true;
    }
    for (auto opt: options)
        free(opt);
    options.clear();
    if (o->image)
    {
        bsd->watch = NULL;
        vitastor_c_watch_inode(bsd->cli, o->image, watch_callback, bsd);
        if (!bsd->epoll_based)
        {
            while (true)
            {
                vitastor_c_uring_handle_events(bsd->cli);
                if (bsd->watch)
                    break;
                vitastor_c_uring_wait_events(bsd->cli);
            }
        }
        else
        {
            while (true)
            {
                if (bsd->watch)
                    break;
                vitastor_c_epoll_handle_events(bsd->cli, 1000);
            }
        }
        td->files[0]->real_file_size = vitastor_c_inode_get_size(bsd->watch);
        if (!vitastor_c_inode_get_num(bsd->watch) ||
            !td->files[0]->real_file_size)
        {
            td_verror(td, EINVAL, "image does not exist");
            return 1;
        }
    }

    bsd->trace = o->trace ? true : false;

    return 0;
}

static void sec_cleanup(struct thread_data *td)
{
    sec_data *bsd = (sec_data*)td->io_ops_data;
    if (bsd)
    {
        if (bsd->mirror_fd >= 0)
        {
            close(bsd->mirror_fd);
        }
        if (bsd->watch)
        {
            vitastor_c_close_watch(bsd->cli, bsd->watch);
        }
        vitastor_c_destroy(bsd->cli);
        delete bsd;
    }
}

/* Connect to the server from each thread. */
static int sec_init(struct thread_data *td)
{
    return 0;
}

static void io_callback(void *opaque, long retval)
{
    struct io_u *io = (struct io_u*)opaque;
    io->error = retval < 0 ? -retval : 0;
    sec_data *bsd = (sec_data*)io->engine_data;
    bsd->inflight--;
    bsd->completed.push_back(io);
    if (bsd->trace)
    {
        printf("--- %s 0x%jx retval=%jd\n", io->ddir == DDIR_READ ? "READ" :
            (io->ddir == DDIR_WRITE ? "WRITE" : "SYNC"), (uint64_t)io, retval);
    }
}

static void read_callback(void *opaque, long retval, uint64_t version)
{
    io_callback(opaque, retval);
}

/* Begin read or write request. */
static enum fio_q_status sec_queue(struct thread_data *td, struct io_u *io)
{
    sec_options *opt = (sec_options*)td->eo;
    sec_data *bsd = (sec_data*)td->io_ops_data;
    struct iovec iov;

    fio_ro_check(td, io);
    if (io->ddir == DDIR_SYNC && bsd->last_sync)
    {
        return FIO_Q_COMPLETED;
    }

    io->engine_data = bsd;
    io->error = 0;
    bsd->inflight++;

    uint64_t inode = opt->image ? vitastor_c_inode_get_num(bsd->watch) : opt->inode;
    assert(io->xfer_buflen < (size_t)-1);
    switch (io->ddir)
    {
    case DDIR_READ:
        iov = { .iov_base = io->xfer_buf, .iov_len = (size_t)io->xfer_buflen };
        vitastor_c_read(bsd->cli, inode, io->offset, io->xfer_buflen, &iov, 1, read_callback, io);
        bsd->last_sync = false;
        break;
    case DDIR_WRITE:
        if (opt->mirror_file)
        {
            size_t done = 0;
            while (done < io->xfer_buflen)
            {
                ssize_t r = pwrite(bsd->mirror_fd, (uint8_t*)io->xfer_buf+done, io->xfer_buflen-done, io->offset+done);
                if (r < 0 && errno != EAGAIN)
                {
                    fprintf(stderr, "Error writing mirror file: %s\n", strerror(errno));
                    io->error = errno;
                    return FIO_Q_COMPLETED;
                }
                if (r > 0)
                {
                    done += r;
                }
            }
        }
        if (opt->image && vitastor_c_inode_get_readonly(bsd->watch))
        {
            io->error = EROFS;
            return FIO_Q_COMPLETED;
        }
        iov = { .iov_base = io->xfer_buf, .iov_len = (size_t)io->xfer_buflen };
        vitastor_c_write(bsd->cli, inode, io->offset, io->xfer_buflen, 0, &iov, 1, io_callback, io);
        bsd->last_sync = false;
        break;
    case DDIR_SYNC:
        vitastor_c_sync(bsd->cli, io_callback, io);
        bsd->last_sync = true;
        break;
    default:
        io->error = EINVAL;
        return FIO_Q_COMPLETED;
    }

    if (opt->trace)
    {
        if (io->ddir == DDIR_SYNC)
        {
            printf("+++ SYNC 0x%jx\n", (uint64_t)io);
        }
        else
        {
            printf("+++ %s 0x%jx 0x%llx+%jx\n",
                io->ddir == DDIR_READ ? "READ" : "WRITE",
                (uint64_t)io, io->offset, (uint64_t)io->xfer_buflen);
        }
    }

    if (io->error != 0)
        return FIO_Q_COMPLETED;
    return FIO_Q_QUEUED;
}

static int sec_getevents(struct thread_data *td, unsigned int min, unsigned int max, const struct timespec *t)
{
    sec_data *bsd = (sec_data*)td->io_ops_data;
    if (!bsd->epoll_based)
    {
        while (true)
        {
            vitastor_c_uring_handle_events(bsd->cli);
            if (bsd->completed.size() >= min)
                break;
            vitastor_c_uring_wait_events(bsd->cli);
        }
    }
    else
    {
        while (true)
        {
            if (bsd->completed.size() >= min)
                break;
            vitastor_c_epoll_handle_events(bsd->cli, 1000);
        }
    }
    return bsd->completed.size();
}

static struct io_u *sec_event(struct thread_data *td, int event)
{
    sec_data *bsd = (sec_data*)td->io_ops_data;
    if (bsd->completed.size() == 0)
        return NULL;
    /* FIXME We ignore the event number and assume fio calls us exactly once for [0..nr_events-1] */
    struct io_u *ev = bsd->completed.back();
    bsd->completed.pop_back();
    return ev;
}

static int sec_io_u_init(struct thread_data *td, struct io_u *io)
{
    io->engine_data = NULL;
    return 0;
}

static void sec_io_u_free(struct thread_data *td, struct io_u *io)
{
}

static int sec_open_file(struct thread_data *td, struct fio_file *f)
{
    return 0;
}

static int sec_invalidate(struct thread_data *td, struct fio_file *f)
{
    return 0;
}

struct ioengine_ops __attribute__((visibility("default"))) ioengine = {
    .name               = "vitastor_cluster",
    .version            = FIO_IOOPS_VERSION,
    .flags              = FIO_MEMALIGN | FIO_DISKLESSIO | FIO_NOEXTEND,
    .setup              = sec_setup,
    .init               = sec_init,
    .queue              = sec_queue,
    .getevents          = sec_getevents,
    .event              = sec_event,
    .cleanup            = sec_cleanup,
    .open_file          = sec_open_file,
    .invalidate         = sec_invalidate,
    .io_u_init          = sec_io_u_init,
    .io_u_free          = sec_io_u_free,
    .option_struct_size = sizeof(struct sec_options),
    .options            = options,
};

static void fio_init fio_sec_register(void)
{
    register_ioengine(&ioengine);
}

static void fio_exit fio_sec_unregister(void)
{
    unregister_ioengine(&ioengine);
}
