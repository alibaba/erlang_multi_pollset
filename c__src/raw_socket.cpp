#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/queue.h>
#include <ev.h>
#include <stack>
#include <vector>

#include "erl_nif.h"
#include "erl_driver.h" /* Included for extern char* erl_errno_id(int error); */

#ifdef __GNUC__
#define __aligned(x) __attribute__((__aligned__(x)))
#else
#define __aligned(x)
#endif

#ifdef DEBUG
#define DBG(format, ...) \
    fprintf(stdout, format, ##__VA_ARGS__)
#else
#define DBG(format, ...)
#endif

/* general address encode/decode tag */
#define INET_AF_INET        1
#define INET_AF_INET6       2
#define INET_AF_ANY         3
#define INET_AF_LOOPBACK    4

#define ACTIVE_FALSE        0
#define ACTIVE_TRUE        -1
#define ACTIVE_ONCE        -2

#define MIGRATE_KEEP        7

#define TCP_MAX_PACKET_SIZE 0x4000000  /* 64 M */

#define MAX_BINARY_LIST_SIZE 0x10000000 /* 256M */
/* How to bind with PollSet
 * 0: Round-Robin
 * 1: Least count
 */
#define BIND_POLICY_RR      0
#define BIND_POLICY_LC      1

static ERL_NIF_TERM TermOk;
static ERL_NIF_TERM TermError;
static ERL_NIF_TERM TermTcpError;
static ERL_NIF_TERM TermClosed;
static ERL_NIF_TERM TermTcpClosed;
static ERL_NIF_TERM TermRead;
static ERL_NIF_TERM TermWrite;
static ERL_NIF_TERM TermData;
static ERL_NIF_TERM TermPassive;
static ERL_NIF_TERM TermSocketEv;
static ERL_NIF_TERM TermPSEv;
static ERL_NIF_TERM TermUndefined;
static ERL_NIF_TERM TermId;
static ERL_NIF_TERM TermTcp;
static ERL_NIF_TERM TermSocketBindId;
static ERL_NIF_TERM TermReadActive;
static ERL_NIF_TERM TermWriteActive;
static ERL_NIF_TERM TermActiveN;
static ERL_NIF_TERM TermActiveSetting;
static ERL_NIF_TERM TermFD;
static ERL_NIF_TERM TermBindPolicy;
static ERL_NIF_TERM TermSocketOnline;
static ERL_NIF_TERM TermBinaryListSize;

static ErlNifResourceType *ResTypeSock;
static ErlNifResourceType *ResTypeBuff;
static ErlNifResourceType *ResTypePSMon;


#define EV_WORKING  0x01
#define EV_WANT     0x02
#define EV_AUX      0x04

#define INIT_PID(p) bzero(&p, sizeof(p))

typedef unsigned long long UINT64;

enum SOCKET_STAT {
    gen_socket_stat_recv_cnt = 0,
    gen_socket_stat_recv_max,
    gen_socket_stat_recv_avg,
    gen_socket_stat_recv_dvi,
    gen_socket_stat_recv_oct,

    gen_socket_stat_send_cnt,
    gen_socket_stat_send_max,
    gen_socket_stat_send_avg,
    gen_socket_stat_send_pend,
    gen_socket_stat_send_oct,
    SOCKET_STAT_ITEMS
};

/* All item's size is 8 byte
 *
 * recv_avg, recv_dvi, send_avg store as double type in SocketResource,
 * and returned as UINT64 to caller.
 */
union SocketStat {
    struct {
        UINT64 recv_cnt;
        UINT64 recv_max;
        double recv_avg;
        double recv_dvi;
        UINT64 recv_oct;

        UINT64 send_cnt;
        UINT64 send_max;
        double send_avg;
        UINT64 send_pend;
        UINT64 send_oct;
    } socket_stat;
    UINT64 v[SOCKET_STAT_ITEMS];
};

struct AddrInfo {
    struct sockaddr_in addr;
    unsigned char bytes[sizeof(struct sockaddr_in)];
};

/*
 * Lock order:
 * socket rwlock -> pollset lock
 *
 * socket rwlock : s
 * pollset lock  : p
 *
 * When destroying or closing the socket, writer lock should be holden instead
 * of reader lock, also EV_WORKING should be checked to see if ev loop is
 * working on the socket.
 *
 * See also:
 *  atomic_rdlock_fd_lock_ps()
 *  atomic_wrlock_fd_lock_ps()
 */
struct SocketResource {
    int fd;            /* s */
    ErlNifRWLock *lock;
    ErlNifCond *ev_cond;
    ev_io ev_read;     /* p */
    ev_io ev_write;    /* p */
    ErlNifPid owner;   /* s + p */
    ErlNifPid ev_caller; /* s + p */
    int evflags;       /* p */
    /*
     * active_setting:
     * 0  - {active, false}
     * -1 - {active, true}
     * -2 - {active, once}
     */
    int active_n;      /* p */
    int active_setting;/* p */
    int pollset;       /* s + p */
    int pollset_setting;    /* s + p */
    volatile int migrate_keep;
    int send_timeout;

    SocketStat stat;
    AddrInfo *peer_ptr, *sock_ptr;
    AddrInfo peer_info, sock_info;
};

struct BufferResource {
    void *buffer;
    int capacity;   /* total bytes we want to receive */
    int len;        /* current recieved bytes */
    int size;       /* maximum space in the buffer */
    int fixed;      /* the buffer was converted to binary, can not be changed */
};

struct PSMonResource {
    int pollset;    /* pollset index */
    ErlNifPid owner;
    TAILQ_ENTRY(PSMonResource) next;
    int on_list;
};

struct PollSet {
    struct ev_loop *loop;
    ErlNifMutex *lock;
    ErlNifCond *cond;
    ev_async async_w;
    int stop;
    int running;
    ErlNifMutex *msg_lock;
    ErlNifEnv* msg_env;
    int socket_online;
    ErlNifTid aux_thread;
    TAILQ_HEAD(, PSMonResource) mon_list;
} __aligned(64);

/*
 * A class to automatically unlock socket on destruction.
 */
class FileLocker {
public:
    FileLocker(struct SocketResource *fd);
    ~FileLocker();

private:
    SocketResource *m_fd;
};

static __thread int scheduler_id;
#if 0
static __thread int invoke_maxcnt;
static __thread int pending_cnt_save;
#endif
static int ncpus;
static struct PollSet *pollsets;
static unsigned next_pollset;
static ErlNifMutex *next_pollset_lock;
/* How to bind with PollSet
 * 0: Round-Robin
 * 1: Pick the least socket_online from all PollSets
 */
static int bind_policy;

#define get_string(env, src, dst, size) \
    enif_get_string(env, src, dst, size, ERL_NIF_LATIN1)
#define get_atom(env, src, dst, size) \
    enif_get_atom(env, src, dst, sizeof(dst), ERL_NIF_LATIN1)
#define make_posix_error(env, err)  \
    enif_make_tuple2(env, TermError, enif_make_atom(env, erl_errno_id(err)))

static int read_packet(SocketResource *handle, ErlNifBinary *bin, int *error);
static ERL_NIF_TERM set_active_n(ErlNifEnv *env, const ERL_NIF_TERM argv[],
                                 int active_n);
static ERL_NIF_TERM make_handle_tuple2(ErlNifEnv *env,
                                       SocketResource *handle);
static void close_by_poller(struct SocketResource *handle, struct PollSet *ps);
static void clear_socketresource(PollSet *ps, SocketResource *handle);
static void update_recv_count(SocketResource *fileHandle, ErlDrvSizeT len);
static void update_send_count(SocketResource *fileHandle, ErlDrvSizeT len);
static int bind_pollset(struct SocketResource *handle, int pollset,
    int autoMigrate);
static void try_to_migrate(struct SocketResource *handle, int imm);
extern "C" int ev_invoke_pending2 (EV_P, int maxcnt);


static inline void
get_next_pollset(SocketResource *s)
{
    assert((bind_policy == BIND_POLICY_RR) || (bind_policy == BIND_POLICY_LC));

    if (bind_policy == BIND_POLICY_RR) {
        unsigned set = __sync_add_and_fetch(&next_pollset, 1);
        s->pollset = set % (unsigned)ncpus;
        s->pollset_setting = -1;
        __sync_add_and_fetch(&pollsets[s->pollset].socket_online, 1);
    } else if (bind_policy == BIND_POLICY_LC) {
        int i;
        int index = 0;
        int min = pollsets[0].socket_online;
        for (i = 1; i < ncpus; i++) {
            if (pollsets[i].socket_online <= min) {
                min = pollsets[i].socket_online;
                index = i;
            }
        }
        __sync_add_and_fetch(&pollsets[index].socket_online, 1);
        s->pollset = index;
        s->pollset_setting = index;
    }
}

static inline struct PollSet *
get_fd_ps(struct SocketResource *s)
{
    return &pollsets[s->pollset];
}

static inline void
lock_ps(struct PollSet *ps)
{
    enif_mutex_lock(ps->lock);
}

static inline void
unlock_ps(struct PollSet *ps)
{
    enif_mutex_unlock(ps->lock);
}

static inline void
lock_msg(struct PollSet *ps)
{
    enif_mutex_lock(ps->msg_lock);
}

static inline void
unlock_msg(struct PollSet *ps)
{
    enif_mutex_unlock(ps->msg_lock);
}

static inline void
ps_notify(struct PollSet *ps)
{
    ev_async_send(ps->loop, &ps->async_w);
}

static inline void
rdlock_fd(struct SocketResource *fd)
{
    enif_rwlock_rlock(fd->lock);
}

static inline void
rdunlock_fd(struct SocketResource *fd)
{
    enif_rwlock_runlock(fd->lock);
}

static inline void
wrlock_fd(struct SocketResource *fd)
{
    enif_rwlock_rwlock(fd->lock);
}

static inline void
wrunlock_fd(struct SocketResource *fd)
{
    enif_rwlock_rwunlock(fd->lock);
}

FileLocker::FileLocker(struct SocketResource *fd)
:m_fd(fd)
{
    rdlock_fd(fd);
}

FileLocker::~FileLocker()
{
    rdunlock_fd(m_fd);
}

static inline void
wait_ps(struct PollSet *ps, struct SocketResource *handle)
{
    while (handle->evflags & EV_WORKING) {
        handle->evflags |= EV_WANT;
        enif_cond_wait(handle->ev_cond, ps->lock);
    }
}

static inline void
remove_mon_list(struct PollSet *ps, struct PSMonResource *handle)
{
    if (handle->on_list) {
        TAILQ_REMOVE(&ps->mon_list, handle, next);
        handle->on_list = 0;
    }
}

static inline void
insert_mon_list(struct PollSet *ps, struct PSMonResource *handle)
{
    if (!handle->on_list) {
        TAILQ_INSERT_TAIL(&ps->mon_list, handle, next);
        handle->on_list = 1;
    }
}

static inline void
clear_socketresource(PollSet *ps, SocketResource *handle)
{
    if (handle->fd != -1) {
        DBG("File destructor ok %s %d\n", __func__, handle->fd);
        close(handle->fd);
        handle->fd = -1;
        handle->peer_ptr = NULL;
        handle->sock_ptr = NULL;
        __sync_add_and_fetch(&ps->socket_online, -1);
    } else {
        DBG("File destructor failed %s %d\n", __func__, handle->fd);
    }
}

static void
file_dtor(ErlNifEnv* env, void* obj)
{
    struct SocketResource *handle = static_cast<SocketResource *>(obj);
    struct PollSet *ps;

    ps = get_fd_ps(handle);
    lock_ps(ps);
    wait_ps(ps, handle);
    clear_socketresource(ps, handle);
    ev_io_stop(ps->loop, &handle->ev_read);
    ev_io_stop(ps->loop, &handle->ev_write);
    unlock_ps(ps);

    enif_rwlock_destroy(handle->lock);
    enif_cond_destroy(handle->ev_cond);
}

static void
psmon_dtor(ErlNifEnv* env, void* obj)
{
    struct PSMonResource *handle = static_cast<PSMonResource *>(obj);
    struct PollSet *ps;

    DBG("PSMon destructor %s\n", __func__);
    ps = &pollsets[handle->pollset];
    lock_ps(ps);
    remove_mon_list(ps, handle);
    unlock_ps(ps);
}

static inline void atomic_rdlock_fd_lock_ps(struct SocketResource *handle)
{
    struct PollSet *ps;

again:
    rdlock_fd(handle);
    ps = get_fd_ps(handle);
    lock_ps(ps);
    if (handle->evflags & EV_WORKING) {
        rdunlock_fd(handle);
        while (handle->evflags & EV_WORKING) {
            handle->evflags |= EV_WANT;
            enif_cond_wait(handle->ev_cond, ps->lock);
        }
        unlock_ps(ps);
        goto again;
    }
}

static inline void atomic_wrlock_fd_lock_ps(struct SocketResource *handle)
{
    struct PollSet *ps;

again:
    wrlock_fd(handle);
    ps = get_fd_ps(handle);
    lock_ps(ps);
    if (handle->evflags & EV_WORKING) {
        wrunlock_fd(handle);
        while (handle->evflags & EV_WORKING) {
            handle->evflags |= EV_WANT;
            enif_cond_wait(handle->ev_cond, ps->lock);
        }
        unlock_ps(ps);
        goto again;
    }
}

static void
ev_write_cb(struct ev_loop *loop, ev_io *ev, int revents)
{
    struct SocketResource *handle = (struct SocketResource *)(ev->data);
    struct PollSet *ps = (struct PollSet *)ev_userdata(loop);
    ERL_NIF_TERM term_tuple;

    ev_io_stop(loop, ev);
    ErlNifPid caller = handle->ev_caller;
    INIT_PID(handle->ev_caller);
    handle->evflags |= EV_WORKING; // we are working
    unlock_ps(ps);

    ErlNifEnv* msg_env = ps->msg_env;
    lock_msg(ps);
    term_tuple = enif_make_tuple3(msg_env,
        TermSocketEv,
        (revents & EV_WRITE) ? TermWrite : TermRead,
        make_handle_tuple2(msg_env, handle));

    enif_send(NULL, &caller, msg_env, term_tuple);
    enif_clear_env(msg_env);
    unlock_msg(ps);

    lock_ps(ps);
    handle->evflags &= ~EV_WORKING;
    if (handle->evflags & EV_WANT) {
        handle->evflags &= ~EV_WANT;
        enif_cond_broadcast(handle->ev_cond);
    }
}

static void
ev_read_cb(struct ev_loop *loop, ev_io *ev, int revents)
{
    struct SocketResource *handle = (struct SocketResource *)(ev->data);
    struct PollSet *ps = (struct PollSet *)ev_userdata(loop);
    ERL_NIF_TERM term_tuple;
    ErlNifEnv* msg_env = ps->msg_env;
    ErlNifPid owner;
    ErlNifBinary bin;

    owner = handle->owner;
    handle->evflags |= EV_WORKING; // we are working
    /*
     * (handle->active_setting < 0) equals
     *     (handle->active_setting == ACTIVE_TRUE || handle->active_setting == ACTIVE_ONCE)
     */
    if (handle->active_setting < 0 || handle->active_n) {
        if (handle->active_setting > 0)
            --handle->active_n;
        unlock_ps(ps);
        int error;
        int rc = read_packet(handle, &bin, &error);
        if (rc > 0) {
            lock_msg(ps);
            term_tuple = enif_make_tuple3(msg_env, TermTcp,
                                          make_handle_tuple2(msg_env, handle),
                                          enif_make_binary(msg_env, &bin));
            enif_send(NULL, &owner, msg_env, term_tuple);
            enif_clear_env(msg_env);
            if (handle->active_n == 0) { /* handle->active_n == 0 only when active==true */
                term_tuple = enif_make_tuple2(msg_env, TermPassive,
                                make_handle_tuple2(msg_env, handle));
                enif_send(NULL, &owner, msg_env, term_tuple);
                enif_clear_env(msg_env);
            }
            unlock_msg(ps);
            update_recv_count(handle, bin.size);
        } else if (rc == -2) { /* closed */
            lock_msg(ps);
            term_tuple = enif_make_tuple2(msg_env, TermTcpClosed,
                            make_handle_tuple2(msg_env, handle));
            enif_send(NULL, &owner, msg_env, term_tuple);
            enif_clear_env(msg_env);
            unlock_msg(ps);
            handle->active_n = 0;
            close_by_poller(handle, ps);
        } else if (rc == -1) { /* error */
            lock_msg(ps);
            term_tuple = enif_make_tuple3(msg_env, TermTcpError,
                            make_handle_tuple2(msg_env, handle),
                            enif_make_atom(msg_env, erl_errno_id(error)));
            enif_send(NULL, &owner, msg_env, term_tuple);
            enif_clear_env(msg_env);
            unlock_msg(ps);
            handle->active_n = 0;
            close_by_poller(handle, ps);
        } else {
            assert(rc == 0);
            if (handle->active_setting > 0)
                handle->active_n++; // recover, rarely happen!
        }
        lock_ps(ps);
        if (handle->active_n == 0 ||
            (rc != 0 && handle->active_setting == ACTIVE_ONCE))
            ev_io_stop(loop, ev);
    } else {
        ErlNifPid caller = handle->ev_caller;
        INIT_PID(handle->ev_caller);
        ev_io_stop(loop, ev);
        unlock_ps(ps);
        lock_msg(ps);
        term_tuple = enif_make_tuple3(msg_env,
            TermSocketEv, TermRead,
            make_handle_tuple2(msg_env, handle));
        enif_send(NULL, &caller, msg_env, term_tuple);
        enif_clear_env(msg_env);
        unlock_msg(ps);
        lock_ps(ps);
    }
    handle->evflags &= ~EV_WORKING;
    if (handle->evflags & EV_WANT) {
        handle->evflags &= ~EV_WANT;
        enif_cond_broadcast(handle->ev_cond);
    }
}

static inline ERL_NIF_TERM
init_socketresource(ErlNifEnv *env, int fd)
{
    struct SocketResource *handle;

    handle = (struct SocketResource *)
        enif_alloc_resource(ResTypeSock, sizeof(struct SocketResource));
    bzero(handle, sizeof(*handle));
    get_next_pollset(handle);
    handle->fd = fd;
    handle->lock = enif_rwlock_create(const_cast<char *>("evfile"));
    handle->ev_cond = enif_cond_create(const_cast<char *>("evfile"));

    ev_io_init(&handle->ev_read, ev_read_cb, fd, EV_READ);
    handle->ev_read.data = handle;
    ev_io_init(&handle->ev_write, ev_write_cb, fd, EV_WRITE);
    handle->ev_write.data = handle;
    enif_self(env, &handle->owner);
    INIT_PID(handle->ev_caller);
    handle->evflags = 0;
    handle->active_n = handle->active_setting = 0;
    bzero(&handle->stat, sizeof(SocketStat));
    handle->peer_ptr = NULL;
    handle->sock_ptr = NULL;

    ERL_NIF_TERM handle_term = enif_make_resource(env, handle);
    /* after release, the resource only owned by erlang */
    enif_release_resource(handle);

    return enif_make_tuple2(env, enif_make_long(env, (long)handle), handle_term);
}

static inline void
update_recv_count(SocketResource *fileHandle, ErlDrvSizeT len)
{
    UINT64 n = ++fileHandle->stat.socket_stat.recv_cnt;

    if (len > fileHandle->stat.socket_stat.recv_max) {
        fileHandle->stat.socket_stat.recv_max = len;
    }

    double avg = fileHandle->stat.socket_stat.recv_avg;
    avg = avg + (len - avg) / n;
    fileHandle->stat.socket_stat.recv_avg = avg;

    double dvi = fileHandle->stat.socket_stat.recv_dvi;
    fileHandle->stat.socket_stat.recv_dvi = dvi + ((len - avg) - dvi) / n;

    fileHandle->stat.socket_stat.recv_oct += len;
}

static inline void
update_send_count(SocketResource *fileHandle, ErlDrvSizeT len)
{
    UINT64 n = ++fileHandle->stat.socket_stat.send_cnt;

    if (len > fileHandle->stat.socket_stat.send_max) {
        fileHandle->stat.socket_stat.send_max = len;
    }

    double avg = fileHandle->stat.socket_stat.send_avg;
    avg = avg + (len - avg) / n;
    fileHandle->stat.socket_stat.send_avg = avg;

    fileHandle->stat.socket_stat.send_oct += len;
}

static void
buff_dtor(ErlNifEnv *env, void *obj)
{
    struct BufferResource *handle = static_cast<BufferResource *>(obj);

    enif_free(handle->buffer);
    handle->buffer = 0;
    handle->len = 0;
    handle->capacity = 0;
}

static inline int
set_nonblock(int fd)
{
    int fflags;

    fflags = fcntl(fd, F_GETFL, 0);
    if (fflags == -1)
        return -1;
    fflags |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, fflags);
}

static inline int
set_nodelay(int fd, int nodelay)
{
    int opt;

    opt = !!nodelay;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

static inline int
get_nodelay(int fd, int *nodelay)
{
    socklen_t len;

    len = sizeof(int);
    return getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, nodelay, &len);
}

static inline int
set_reuseaddr(int fd, int reuse)
{
    int opt;

    opt = !!reuse;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

static inline int
set_keepalive(int fd, int keep)
{
    int opt;

    opt = !!keep;
    return setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
}

static inline int
set_rcvbuf(int fd, int rcvbuf)
{
    return setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
}

static inline int
set_sndbuf(int fd, int sndbuf)
{
    return setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
}

static inline int
get_reuseaddr(int fd, int *reuse)
{
    socklen_t len;

    len = sizeof(int);
    return getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reuse, &len);
}

static inline int
get_socket_error(int fd, int *error)
{
    socklen_t len;

    len = sizeof(int);
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, error, &len);
}

static inline ERL_NIF_TERM
make_handle_tuple2(ErlNifEnv *env, SocketResource *handle)
{
    return enif_make_tuple2(env,
                            enif_make_long(env, (long)handle),
                            enif_make_resource(env, handle));
}

static ERL_NIF_TERM
nif_socket(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int fd;

    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1)
        return make_posix_error(env, errno);
    if (set_nonblock(fd)) {
        int err = errno;
        close(fd);
        return make_posix_error(env,  err);
    }

    ERL_NIF_TERM handle_term_tuple = init_socketresource(env, fd);

    return enif_make_tuple2(env, TermOk, handle_term_tuple);
}

#define PREPARE_GET_HANDLE                                              \
    const ERL_NIF_TERM *array_internal;                                 \
    int arity_internal;                                                 \
    if (enif_is_tuple(env, argv[0])) {                                  \
        if (!enif_get_tuple(env, argv[0], &arity_internal, &array_internal)) \
            return enif_make_badarg(env);                               \
        if (arity_internal != 2)                                        \
            return enif_make_badarg(env);                               \
                                                                        \
        if (!enif_get_resource(env, array_internal[1],                  \
                               ResTypeSock, (void **)&handle))          \
            return enif_make_badarg(env);                               \
    } else {                                                            \
        return enif_make_badarg(env);                                   \
    }

#define PREPARE_GET_FD                          \
    PREPARE_GET_HANDLE                          \
    FileLocker flocker(handle);                 \
    fd = handle->fd;                            \
    if (fd == -1)                               \
        return enif_make_tuple2(env, TermError, TermClosed);

static int get_bool(ErlNifEnv *env, ERL_NIF_TERM term, int *v)
{
    char atom[16];

    if (!get_atom(env, term, atom, sizeof(atom)))
        return 0;
    if (strcmp(atom, "true") == 0) {
        *v = 1;
        return 1;
    }
    if (strcmp(atom, "false") == 0) {
        *v = 0;
        return 1;
    }
    return 0; // invalid
}

static ERL_NIF_TERM
nif_setsockopt(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    char atom[64];
    struct SocketResource *handle;
    const ERL_NIF_TERM* array;
    int fd, ival, arity;

    if (enif_is_tuple(env, argv[1])) {
        if (!enif_get_tuple(env, argv[1], &arity, &array))
            return enif_make_badarg(env);
        if (arity != 2)
            return enif_make_badarg(env);
        if (!get_atom(env, array[0], atom, sizeof(atom)))
            return enif_make_badarg(env);
        if (!strcmp(atom, "active")) {
            if (!enif_get_int(env, array[1], &ival))
                return enif_make_badarg(env);
            return set_active_n(env, argv, ival);
        }
        if (!strcmp(atom, "nodelay")) {
            PREPARE_GET_FD
            if (!get_bool(env, array[1], &ival))
                return enif_make_badarg(env);
            if (!set_nodelay(fd, ival))
                return TermOk;
            return make_posix_error(env, errno);
        }
        if (!strcmp(atom, "reuseaddr")) {
            PREPARE_GET_FD
            if (!get_bool(env, array[1], &ival))
                return enif_make_badarg(env);
            if (!set_reuseaddr(fd, ival))
                return TermOk;
            return make_posix_error(env, errno);
        }
        if (!strcmp(atom, "keepalive")) {
            PREPARE_GET_FD
            if (!get_bool(env, array[1], &ival))
                return enif_make_badarg(env);
            if (!set_keepalive(fd, ival))
                return TermOk;
            return make_posix_error(env, errno);
        }
        if (!strcmp(atom, "send_timeout")) {
            if (!enif_get_int(env, array[1], &ival))
                return enif_make_badarg(env);
            PREPARE_GET_HANDLE
            handle->send_timeout = ival;
            return TermOk;
        }
        if (!strcmp(atom, "recbuf")) {
            PREPARE_GET_FD
            if (!enif_get_int(env, array[1], &ival))
                return enif_make_badarg(env);
            if (!set_rcvbuf(fd, ival))
                return TermOk;
            return make_posix_error(env, errno);
        }
        if (!strcmp(atom, "sndbuf")) {
            PREPARE_GET_FD
            if (!enif_get_int(env, array[1], &ival))
                return enif_make_badarg(env);
            if (!set_sndbuf(fd, ival))
                return TermOk;
            return make_posix_error(env, errno);
        }
    }
    return enif_make_badarg(env);
}

static ERL_NIF_TERM
nif_getsockopt(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    char atom[64];
    int fd, ival;

    PREPARE_GET_HANDLE
    FileLocker flocker(handle);

    if (!get_atom(env, argv[1], atom, sizeof(atom)))
        return enif_make_badarg(env);

    if (!strcmp(atom, "send_timeout")) {
        return enif_make_tuple2(env, TermOk, enif_make_int(env,
            handle->send_timeout));
    }

    if (!strcmp(atom, "active")) { // active not require fd != -1
        return enif_make_tuple2(env, TermOk,
                    enif_make_int(env, handle->active_setting));
    }

    /*
     * Check fd is valid
     */
    fd = handle->fd;
    if (fd == -1)
        return enif_make_tuple2(env, TermError, TermClosed);

    if (!strcmp(atom, "nodelay")) {
        if (get_nodelay(fd, &ival))
            return make_posix_error(env, errno);
        return enif_make_tuple2(env, TermOk, enif_make_int(env, ival));
    }
    if (!strcmp(atom, "reuseaddr")) {
        if (get_reuseaddr(fd, &ival))
            return make_posix_error(env, errno);
        return enif_make_tuple2(env, TermOk, enif_make_int(env, ival));
    }
    if (!strcmp(atom, "errorcode")) {
        if (get_socket_error(fd, &ival)) // syscall error
            return make_posix_error(env, errno);
        return enif_make_tuple3(env, TermOk, enif_make_int(env, ival),
                                enif_make_atom(env, erl_errno_id(ival)));
    }
    if (!strcmp(atom, "ionread")) {
        if (ioctl(fd, FIONREAD, &ival))
            return make_posix_error(env, errno);
        return enif_make_tuple2(env, TermOk, enif_make_int(env, ival));
    }

    return enif_make_badarg(env);
}

static ERL_NIF_TERM
nif_bind(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    char ip_addr[64];
    struct sockaddr_in sin;
    struct SocketResource *handle;
    int fd, port;

    PREPARE_GET_FD

    if (!get_string(env, argv[1], ip_addr, sizeof(ip_addr)))
        return enif_make_badarg(env);

    if (!enif_get_int(env, argv[2], &port))
        return enif_make_badarg(env);

    if (!inet_aton(ip_addr, &sin.sin_addr))
        return enif_make_badarg(env);

    sin.sin_family = AF_INET;
    sin.sin_port = htons((short)port);
    if (bind(fd, (struct sockaddr*) &sin, sizeof(sin)))
        return make_posix_error(env, errno);
    return TermOk;
}

static ERL_NIF_TERM
nif_listen(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    int fd, backlog;

    PREPARE_GET_FD

    if (!enif_get_int(env, argv[1], &backlog))
        return enif_make_badarg(env);

    if (listen(fd, backlog))
        return make_posix_error(env, errno);
    return TermOk;
}

static ERL_NIF_TERM
nif_accept(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    ERL_NIF_TERM new_handle_term;
    int fd, newfd;

    PREPARE_GET_FD

    newfd = accept(fd, NULL, 0);
    if (newfd != -1) {
        if (set_nonblock(newfd)) {
            int err = errno;
            close(newfd);
            return make_posix_error(env,  err);
        }

        new_handle_term = init_socketresource(env, newfd);

        return enif_make_tuple2(env, TermOk, new_handle_term);
    }

    return make_posix_error(env, errno);
}

static ERL_NIF_TERM
nif_dup_socket(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    ERL_NIF_TERM new_handle_term;
    int fd, newfd;

    PREPARE_GET_FD

    newfd = dup(fd);
    if (newfd != -1) {
        if (set_nonblock(newfd)) {
            int err = errno;
            close(newfd);
            return make_posix_error(env,  err);
        }

        new_handle_term = init_socketresource(env, newfd);

        return enif_make_tuple2(env, TermOk, new_handle_term);
    }

    return make_posix_error(env, errno);
}

static ERL_NIF_TERM
nif_close(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    struct PollSet *ps;

    PREPARE_GET_HANDLE

    atomic_wrlock_fd_lock_ps(handle);
    ps = get_fd_ps(handle);
    ev_io_stop(ps->loop, &handle->ev_read);
    ev_io_stop(ps->loop, &handle->ev_write);
    unlock_ps(ps);
    clear_socketresource(ps, handle);
    wrunlock_fd(handle);
    return TermOk;
}

static void
close_by_poller(SocketResource *handle, struct PollSet *ps)
{

    wrlock_fd(handle);
    lock_ps(ps);
    ev_io_stop(ps->loop, &handle->ev_read);
    ev_io_stop(ps->loop, &handle->ev_write);
    unlock_ps(ps);
    clear_socketresource(ps, handle);
    wrunlock_fd(handle);
    DBG("%d closed by poller\n", fd);
}

static ERL_NIF_TERM
nif_shutdown(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    char atom[64];
    int fd, flag;

    PREPARE_GET_FD

    if (!enif_get_atom(env, argv[1], atom, sizeof(atom), ERL_NIF_LATIN1))
        return enif_make_badarg(env);

    flag = 0;
    if (!strcmp(atom, "read"))
        flag = SHUT_RD;
    else if (!strcmp(atom, "write"))
        flag = SHUT_WR;
    else if (!strcmp(atom, "read_write"))
        flag = SHUT_RDWR;
    else
        return enif_make_badarg(env);

    if (shutdown(fd, flag))
        return make_posix_error(env, errno);
    return TermOk;
}

static int read_packet(SocketResource *handle, ErlNifBinary *bin, int *error)
{
    int fd = handle->fd;
    int count;

    if (ioctl(fd, FIONREAD, &count)) {
        *error = errno;
        return -1;
    }

    if (count == 0)
        count = 1;
    if (!enif_alloc_binary(count, bin)) {
        *error = ENOMEM;
        return -1;
    }

    char *p = (char *) bin->data;
    int eof = 0;
    int total = 0;
    while (count > 0) {
        int loaded = read(fd, p, count);
        if (loaded < 0) {
            int err = errno;
            if (err == EINTR)
                continue;
            if (err == EAGAIN)
                break;
            enif_release_binary(bin);
            *error = err;
            return -1;
        }
        if (loaded == 0) {
            eof = 1;
            break;
        }
        count -= loaded;
        p += loaded;
        total += loaded;
    }

    if (total != 0) {
        if (count != 0) {
           if (!enif_realloc_binary(bin, total)) {
                enif_release_binary(bin);
                *error = ENOMEM;
                return -1;
           }
        }
    } else {
        enif_release_binary(bin);
        if (eof)
            return -2;
        return 0;
    }
    return 1;
}

static ERL_NIF_TERM
nif_recv(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    ErlNifBinary bin;
    int fd, error, rc;

    PREPARE_GET_FD

    if (handle->active_setting != ACTIVE_FALSE) {
        DBG("recv while active setting is not false!");
        return make_posix_error(env, EINVAL);
    }
    rc = read_packet(handle, &bin, &error);
    if (rc > 0) {
        update_recv_count(handle, rc);
        return enif_make_tuple2(env, TermOk, enif_make_binary(env, &bin));
    } else if (rc == 0)
        return make_posix_error(env, EAGAIN);
    else if (rc == -1)
        return make_posix_error(env, error);
    assert(rc == -2);
    return enif_make_tuple2(env, TermError, TermClosed);
}

static ERL_NIF_TERM
nif_send(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    ErlNifBinary bin;
    int fd, off, len;

    PREPARE_GET_FD

    if (!enif_inspect_binary(env, argv[1], &bin) ||
        !enif_get_int(env, argv[2], &off) ||
        !enif_get_int(env, argv[3], &len)) {
        return enif_make_badarg(env);
    }

    if (off < 0 || len < 0 || (unsigned) off > bin.size)
        return enif_make_badarg(env);
    if (off + len > (int)bin.size)
        len = bin.size - off;

    unsigned char *p = bin.data + off;
    int total = 0;

    while (len > 0) {
        int sent = send(fd, p, len, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                if (total == 0)
                    return make_posix_error(env, errno);
                else
                    break;
            }
            return make_posix_error(env, errno);
        }

        len -= sent;
        p += sent;
        total += sent;
    }

    update_send_count(handle, total);

    return enif_make_tuple3(env, TermOk,
            enif_make_int(env, p - bin.data),
            enif_make_int(env, bin.size)
            );
}

static ERL_NIF_TERM
do_iolist_to_binary_list(ErlNifEnv *env, const ERL_NIF_TERM iolist)
{
    std::stack<ERL_NIF_TERM> s;
    ERL_NIF_TERM cur;
    ERL_NIF_TERM car, cdr;
    ErlNifBinary bin;
    ERL_NIF_TERM result = enif_make_list(env, 0);
    int total = 0;

    s.push(iolist);
    while(!s.empty()) {
        cur = s.top();
        s.pop();

        while (enif_get_list_cell(env, cur, &car, &cdr)) {
            if (enif_is_number(env, car)) {
                std::vector<char> bytes;
                bytes.reserve(256);
                while (enif_get_list_cell(env, cur, &car, &cdr) &&  enif_is_number(env, car)) {
                    int r;
                    enif_get_int(env, car, &r);
                    if (enif_get_int(env, car, &r) && (r >= 0 && r < 256))
                        bytes.push_back((char)r);
                    else
                        return  enif_make_badarg(env);
                    cur = cdr;
                }

                if (bytes.size() > 0) {
                    if (total + bytes.size() > MAX_BINARY_LIST_SIZE)
                        return TermBinaryListSize;
                    if (!enif_alloc_binary(bytes.size(), &bin))
                        return make_posix_error(env, ENOMEM);
                    memcpy(bin.data, (char*)&bytes[0], bytes.size());
                    result = enif_make_list_cell(env, enif_make_binary(env, &bin), result);
                    total += bytes.size();
                }
            } else if (enif_is_binary(env, car)) {
                    if (!enif_inspect_binary(env, car, &bin))
                        return enif_make_badarg(env);
                    if (bin.size > 0) {
                        if (total + bin.size > MAX_BINARY_LIST_SIZE)
                            return TermBinaryListSize;
                        result = enif_make_list_cell(env, car, result);
                        total += bin.size;
                    }

                    cur = cdr;
            } else if (enif_is_list(env, car)) {
                s.push(cdr);
                s.push(car);
                break;
            } else
                return enif_make_badarg(env);
        }
    }

    ERL_NIF_TERM binary_list;
    if (enif_make_reverse_list(env, result, &binary_list))
        return enif_make_tuple2(env, binary_list, enif_make_int(env, total));
    else
        return enif_make_badarg(env);
}


static ERL_NIF_TERM
nif_iolist_to_binary_list(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    ERL_NIF_TERM iolist;
    iolist = argv[0];
    return do_iolist_to_binary_list(env, iolist);
}

static inline struct iovec *
forward_iov(struct iovec *iovp, int *iov_len, unsigned int len)
{
    while (iovp->iov_len <= len) {
        len -= iovp->iov_len;
        iovp++;
        --*iov_len;
    }
    if (*iov_len > 0) {
        iovp->iov_base = (char *)iovp->iov_base + len;
        iovp->iov_len -= len;
    }
    return iovp;
}

static ERL_NIF_TERM
nif_sendv(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    ERL_NIF_TERM head, tail;
    int iov_len, iov_len2;
    unsigned int i, size, off, len, total_sent;
    ErlNifBinary bin;
    struct iovec *iovp, *iovp2;
    struct msghdr msghdr;
    int fd;

    PREPARE_GET_FD

    if (!enif_get_uint(env, argv[2], &off))
        return enif_make_badarg(env);
    tail = argv[1];
    if (!enif_get_list_length(env, tail, (unsigned *)&iov_len))
        return enif_make_badarg(env);
    iovp = (struct iovec *) enif_alloc(iov_len * sizeof(struct iovec));
    if (iovp == NULL)
        return make_posix_error(env, ENOMEM);
    i = 0;
    size = 0;
    while (enif_get_list_cell(env, tail, &head, &tail)) {
        if (!enif_inspect_binary(env, head, &bin)) {
            enif_free(iovp);
            return enif_make_badarg(env);
        }
        iovp[i].iov_base = bin.data;
        iovp[i].iov_len = bin.size;
        size += bin.size;
        i++;
    }

    if (off > size) {
        enif_free(iovp);
        return enif_make_badarg(env);
    } else if (off == size) {
    	goto out;
    }

    len = size - off;
    iov_len2 = iov_len;
    iovp2 = forward_iov(iovp, &iov_len2, off);
    total_sent = 0;
    while (len > 0) {
        int sent;

        bzero(&msghdr, sizeof(msghdr));
        msghdr.msg_iov = iovp2;
        msghdr.msg_iovlen = iov_len2;
        sent = sendmsg(fd, &msghdr, MSG_NOSIGNAL);
        if (sent < 0) {
            int err;

            err = errno;
            if (err == EINTR)
                continue;
            if (err == EAGAIN) {
                if (total_sent == 0) {
                    enif_free(iovp);
                    return make_posix_error(env, err);
                }
                else
                    break;
            }
            enif_free(iovp);
            return make_posix_error(env, err);
        }
        len -= sent;
        off += sent;
        total_sent += sent;
    	if (len > 0)
	        iovp2 = forward_iov(iovp2, &iov_len2, sent);
    }

    update_send_count(handle, total_sent);

out:
    enif_free(iovp);
    return enif_make_tuple3(env, TermOk,
            enif_make_int(env, off),
            enif_make_int(env, size)
            );
}

static ERL_NIF_TERM
nif_ev_info(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    struct PollSet *ps;
    int fd;

    int poller_id;
    int read_active;
    int write_active;
    int active_n;
    int active_setting;

    PREPARE_GET_HANDLE

    atomic_wrlock_fd_lock_ps(handle);

    ps = get_fd_ps(handle);

    poller_id = handle->pollset + 1;

    read_active = handle->ev_read.active;
    write_active = handle->ev_write.active;

    fd = handle->fd;

    active_n = handle->active_n;
    active_setting = handle->active_setting;


    unlock_ps(ps);
    wrunlock_fd(handle);

    //{poller_id+1, read_active, write_active, active_n, active_setting}
    return enif_make_tuple6(env,
                            enif_make_tuple2(env, TermSocketBindId, enif_make_int(env, poller_id)),
                            enif_make_tuple2(env, TermReadActive, enif_make_int(env, read_active)),
                            enif_make_tuple2(env, TermWriteActive, enif_make_int(env, write_active)),
                            enif_make_tuple2(env, TermActiveN, enif_make_int(env, active_n)),
                            enif_make_tuple2(env, TermActiveSetting, enif_make_int(env, active_setting)),
                            enif_make_tuple2(env, TermFD, enif_make_int(env, fd)));
}

static ERL_NIF_TERM
nif_socketonline_info(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{

    ERL_NIF_TERM proplist = enif_make_list(env, 0);
    ERL_NIF_TERM kv;
    int i;
    for (i = 0; i < ncpus; i++) {
        kv = enif_make_tuple2(env, enif_make_int(env, i+1), enif_make_int64(env, pollsets[i].socket_online));
        proplist = enif_make_list_cell(env, kv, proplist);
    }
    return enif_make_tuple2(env,
                            enif_make_tuple2(env, TermBindPolicy, enif_make_int(env, bind_policy)),
                            enif_make_tuple2(env, TermSocketOnline, proplist));
}

static ERL_NIF_TERM
nif_set_bind_policy(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int policy;
    if (!enif_get_int(env, argv[0], &policy))
        return enif_make_badarg(env);
    if ((policy != BIND_POLICY_RR) && (policy != BIND_POLICY_LC))
        return enif_make_badarg(env);

    int old = bind_policy;
    bind_policy = policy;

    // {ok, old_policy_type}
    return enif_make_tuple2(env, TermOk, enif_make_int(env, old));
}


static ERL_NIF_TERM
nif_connect(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    char ip_addr[64];
    struct sockaddr_in sin;
    int fd, port;

    PREPARE_GET_FD

    if (!get_string(env, argv[1], ip_addr, sizeof(ip_addr)))
        return enif_make_badarg(env);

    if (!enif_get_int(env, argv[2], &port))
        return enif_make_badarg(env);

    if (!inet_aton(ip_addr, &sin.sin_addr))
        return enif_make_badarg(env);

    sin.sin_family = AF_INET;
    sin.sin_port = htons((short)port);

    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0)
        return make_posix_error(env, errno);
    return TermOk;
}

static ERL_NIF_TERM
nif_controlling_process(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    struct PollSet *ps;
    ErlNifPid pid, self;

    PREPARE_GET_HANDLE

    if (!enif_get_local_pid(env, argv[1], &pid))
        return enif_make_badarg(env);
    enif_self(env, &self);
    FileLocker flocker(handle);
    ps = get_fd_ps(handle);
    lock_ps(ps);
    if (handle->owner.pid != self.pid) {
        unlock_ps(ps);
        return make_posix_error(env, EPERM);
    }
    handle->owner = pid;
    unlock_ps(ps);
    return TermOk;
}

static ERL_NIF_TERM
do_enable_disable_events(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[],
    int enable)
{
    struct SocketResource *handle;
    struct PollSet *ps;
    ERL_NIF_TERM head, tail;
    ErlNifPid pid;
    char atom[64];
    int events;

    if (enable) {
       if (argc < 3 || !enif_get_local_pid(env, argv[2], &pid))
           return enif_make_badarg(env);
    }

    tail = argv[1];
    events = 0;
    while(enif_get_list_cell(env, tail, &head, &tail)) {
        if (enif_is_atom(env, head)) {
            if (enif_get_atom(env, head, atom, sizeof(atom), ERL_NIF_LATIN1)) {
                if (!strcmp(atom, "readable"))
                    events |= EV_READ;
                else if (!strcmp(atom, "writable"))
                    events |= EV_WRITE;
                else
                    return enif_make_badarg(env);
            }
        }
    }

    PREPARE_GET_HANDLE

    if (enable)
        try_to_migrate(handle, 0);

    atomic_rdlock_fd_lock_ps(handle);
    ps = get_fd_ps(handle);
    if (handle->fd == -1) {
        unlock_ps(ps);
        rdunlock_fd(handle);
        return enif_make_tuple2(env, TermError, TermClosed);
    }

    if ((handle->active_setting != ACTIVE_FALSE) && (events & EV_READ)) {
        unlock_ps(ps);
        rdunlock_fd(handle);
        DBG("enable/disabl event while active setting is not false!");
        return make_posix_error(env, EINVAL);
    }

    if (enable) {
        handle->ev_caller = pid;
        if (events & EV_READ)
            ev_io_start(ps->loop, &handle->ev_read);
        if (events & EV_WRITE)
            ev_io_start(ps->loop, &handle->ev_write);
        rdunlock_fd(handle);
        unlock_ps(ps);
        ps_notify(ps);
    } else {
        if (events & EV_READ)
            ev_io_stop(ps->loop, &handle->ev_read);
        if (events & EV_WRITE)
            ev_io_stop(ps->loop, &handle->ev_write);
        unlock_ps(ps);
        rdunlock_fd(handle);
    }
    return TermOk;
}

static inline void
try_to_migrate(struct SocketResource *handle, int immediate)
{
    return;
    int curmigk = handle->migrate_keep;
    int migrate = 0;

    if (handle->pollset_setting == -1 &&
        handle->pollset != scheduler_id - 1) {
        if (immediate)
            migrate = 1;
        else {
            if (++curmigk == MIGRATE_KEEP) {
                handle->migrate_keep = 0;
                migrate = 1;
            } else {
                handle->migrate_keep = curmigk;
            }
        }
        if (migrate) {
            handle->migrate_keep = 0;
            bind_pollset(handle, -1, 1);
        }
    }
}

static ERL_NIF_TERM
set_active_n(ErlNifEnv *env, const ERL_NIF_TERM argv[], int active_n)
{
    struct SocketResource *handle;
    struct PollSet *ps;
    int old_set;

    PREPARE_GET_HANDLE

    if (active_n < ACTIVE_ONCE)
        return enif_make_badarg(env);

    try_to_migrate(handle, 1);

    /* we use wrlock because in recv() we check active_setting */
    atomic_wrlock_fd_lock_ps(handle);
    ps = get_fd_ps(handle);
    if (handle->fd == -1) {
        unlock_ps(ps);
        wrunlock_fd(handle);
        return enif_make_tuple2(env, TermError, TermClosed);
    }

    old_set = handle->active_setting;
    handle->active_n = handle->active_setting = active_n;
    if (active_n == ACTIVE_FALSE) {
        // XXX should check ps->caller
        if (old_set != ACTIVE_FALSE)
            ev_io_stop(ps->loop, &handle->ev_read);
        unlock_ps(ps);
    } else {
        if (handle->ev_read.active == 0) {
            ev_io_start(ps->loop, &handle->ev_read);
            unlock_ps(ps);
            ps_notify(ps);
        } else {
            unlock_ps(ps);
        }
    }
    wrunlock_fd(handle);

    return TermOk;
}

static ERL_NIF_TERM nif_alloc_buf(ErlNifEnv *env, int argc,
    const ERL_NIF_TERM argv[])
{
    struct BufferResource *handle;
    ERL_NIF_TERM handle_term;
    int cap, size;

    if (!enif_get_int(env, argv[0], &cap) || cap < 0 ||
        !enif_get_int(env, argv[1], &size))
        return enif_make_badarg(env);
    if (size == 0)
        size = cap;
    if (size < cap)
        size = cap;

    if (size > TCP_MAX_PACKET_SIZE)
        return make_posix_error(env, ENOMEM);

    handle = (struct BufferResource *)
        enif_alloc_resource(ResTypeBuff, sizeof(struct BufferResource));
    if (NULL == (handle->buffer = enif_alloc(size))) {
        enif_release_resource(handle);
        return make_posix_error(env, ENOMEM);
    }
    handle->capacity = cap;
    handle->size = size;
    handle->len = 0;
    handle->fixed = 0;
    handle_term = enif_make_resource(env, handle);
    enif_release_resource(handle);
    return enif_make_tuple2(env, TermOk, handle_term);
}

static ERL_NIF_TERM nif_realloc_buf(ErlNifEnv *env, int argc,
    const ERL_NIF_TERM argv[])
{
    struct BufferResource *handle;
    void *newbuf;
    int cap, size;

    if (!enif_get_resource(env, argv[0], ResTypeBuff, (void **)&handle))
        return enif_make_badarg(env);
    if (handle->fixed)
        return make_posix_error(env, EPERM);
    if (!enif_get_int(env, argv[1], &cap) || cap < 0 ||
        !enif_get_int(env, argv[2], &size))
        return enif_make_badarg(env);
    if (size < cap)
        size = cap;
    if (size <= handle->size) {
        handle->capacity = cap;
        if (handle->len > cap)
            handle->len = cap;
        return TermOk;
    }

    newbuf = enif_realloc(handle->buffer, size);
    if (newbuf) {
        handle->buffer = newbuf;
        handle->capacity = cap;
        handle->size = size;
        if (handle->len > cap)
            handle->len = cap;
        return TermOk;
    }

    return make_posix_error(env, ENOMEM);
}

static ERL_NIF_TERM nif_free_buf(ErlNifEnv *env, int argc,
    const ERL_NIF_TERM argv[])
{
    struct BufferResource *handle;

    if (!enif_get_resource(env, argv[0], ResTypeBuff, (void **)&handle))
        return enif_make_badarg(env);
    if (handle->fixed)
        return make_posix_error(env, EINVAL);
    if (handle->buffer != NULL) {
        enif_free(handle->buffer);
        handle->buffer = NULL;
    }
    handle->capacity = handle->len = handle->size = 0;
    return TermOk;
}

static ERL_NIF_TERM nif_get_buf_info(ErlNifEnv *env, int argc,
    const ERL_NIF_TERM argv[])
{
    struct BufferResource *handle;

    if (!enif_get_resource(env, argv[0], ResTypeBuff, (void **)&handle))
        return enif_make_badarg(env);

    return enif_make_tuple4(env, TermOk,
        enif_make_int(env, handle->capacity),
        enif_make_int(env, handle->len),
        enif_make_int(env, handle->size));
}

static ERL_NIF_TERM
nif_recv_buf(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    struct BufferResource *bhandle;
    int fd;

    PREPARE_GET_FD

    if (handle->active_setting != ACTIVE_FALSE) {
        DBG("recv_buf while active setting is not false!");
        return make_posix_error(env, EINVAL);
    }

    if (!enif_get_resource(env, argv[1], ResTypeBuff, (void **)&bhandle))
        return enif_make_badarg(env);

    if (bhandle->buffer == NULL)
        return make_posix_error(env, EINVAL);

    int count = bhandle->capacity - bhandle->len;
    char *p = (char *)bhandle->buffer + bhandle->len;
    int eof = 0;
    int total = 0;
    while (count > 0) {
        int loaded = read(fd, p, count);
        if (loaded < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                break;
            return make_posix_error(env, errno);
        }
        if (loaded == 0) {
            eof = 1;
            break;
        }
        count -= loaded;
        p += loaded;
        total += loaded;
    }

    update_recv_count(handle, total);

    bhandle->len += total;
    if (bhandle->len == bhandle->capacity)
        return TermOk;
    if (eof)
        return enif_make_tuple2(env, TermError, TermClosed);
    return make_posix_error(env, EAGAIN);
}

static ERL_NIF_TERM
nif_get_buf_data(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct BufferResource *handle;
    ERL_NIF_TERM binterm;
    unsigned char *bin;
    int len, off;

    if (!enif_get_resource(env, argv[0], ResTypeBuff, (void **)&handle))
        return enif_make_badarg(env);
    if (!enif_get_int(env, argv[1], &off) || !enif_get_int(env, argv[2], &len))
        return enif_make_badarg(env);
    if (off < 0)
        return enif_make_badarg(env);
    if (off >= handle->len)
        len = 0;
    else if (off + len > handle->len)
        len = handle->len - off;
    bin = enif_make_new_binary(env, len, &binterm);
    if (bin == NULL)
        return make_posix_error(env, ENOMEM);
    memcpy(bin, (char *)handle->buffer + off, len);
    return enif_make_tuple2(env, TermOk, binterm);
}

static ERL_NIF_TERM
nif_convert_buf_to_binary(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct BufferResource *handle;
    ERL_NIF_TERM binterm;

    if (!enif_get_resource(env, argv[0], ResTypeBuff, (void **)&handle))
        return enif_make_badarg(env);
    binterm = enif_make_resource_binary(env, handle, handle->buffer,
        handle->len);
    handle->fixed = 1;
    return enif_make_tuple2(env, TermOk, binterm);
}

static ERL_NIF_TERM nif_enable_events(ErlNifEnv *env, int argc, const
    ERL_NIF_TERM argv[])
{
    return do_enable_disable_events(env, argc, argv, 1);
}

static ERL_NIF_TERM nif_disable_events(ErlNifEnv *env, int argc, const
    ERL_NIF_TERM argv[])
{
    return do_enable_disable_events(env, argc, argv, 0);
}

extern "C" int ev_get_backend_fd(struct ev_loop *loop);

static ERL_NIF_TERM
nif_alloc_psmon(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct PSMonResource *handle;
    ERL_NIF_TERM handle_term;
    int pollset;

    if (!enif_get_int(env, argv[0], &pollset) || pollset < 0 ||
         pollset >= ncpus)
        return enif_make_badarg(env);

    handle = (struct PSMonResource *)
        enif_alloc_resource(ResTypePSMon, sizeof(struct PSMonResource));
    if (handle == NULL)
        return make_posix_error(env, ENOMEM);
    handle->pollset = pollset;
    handle->on_list = 0;
    enif_self(env, &handle->owner);
    handle_term = enif_make_resource(env, handle);
    enif_release_resource(handle);
    return enif_make_tuple2(env, TermOk, handle_term);
}

static void
send_ps_mon(PSMonResource *handle, struct PollSet *ps)
{
    ErlNifPid owner;
    ERL_NIF_TERM term_tuple;
    ErlNifEnv* msg_env = ps->msg_env;

    owner = handle->owner;
    lock_msg(ps);
    term_tuple = enif_make_tuple3(msg_env,
        TermPSEv, TermRead, enif_make_resource(msg_env, handle));
    enif_send(NULL, &owner, msg_env, term_tuple);
    enif_clear_env(msg_env);
    unlock_msg(ps);
}

static ERL_NIF_TERM
set_psmon(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[], int set)
{
    struct PSMonResource *handle;
    struct PollSet *ps;

    if (!enif_get_resource(env, argv[0], ResTypePSMon, (void **)&handle))
        return enif_make_badarg(env);
    ps = &pollsets[handle->pollset];
    lock_ps(ps);
    if (set) {
        if (ev_pending_count(ps->loop))
            send_ps_mon(handle, ps);
        else if (handle->on_list == 0) {
            insert_mon_list(ps, handle);
            enif_cond_broadcast(ps->cond);
        }
    } else {
        remove_mon_list(ps, handle);
    }
    unlock_ps(ps);
    return TermOk;
}

static ERL_NIF_TERM
nif_enable_psmon(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    return set_psmon(env, argc, argv, 1);
}

static ERL_NIF_TERM
nif_disable_psmon(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    return set_psmon(env, argc, argv, 0);
}

static ERL_NIF_TERM
nif_total_pollsets(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    return enif_make_int(env, ncpus);
}

static int
bind_pollset(struct SocketResource *handle, int pollset, int autoMigrate)
{
    struct PollSet *ps;
    int old;

    atomic_wrlock_fd_lock_ps(handle);
    if (autoMigrate && handle->pollset_setting == -1)
        pollset = scheduler_id - 1;
    old = handle->pollset_setting;
    ps = get_fd_ps(handle);
    int read_active = handle->ev_read.active;
    int write_active = handle->ev_write.active;
    if (read_active)
        ev_io_stop(ps->loop, &handle->ev_read);
    if (write_active)
        ev_io_stop(ps->loop, &handle->ev_write);
    unlock_ps(ps);

    __sync_add_and_fetch(&ps->socket_online, -1);

    if (!autoMigrate)
        handle->pollset_setting = pollset;
    if (handle->pollset_setting != -1) {
        /* prebind mode */
        handle->pollset = pollset;
    } else if (scheduler_id != 0) {
        /* auto-migrate mode */
        handle->pollset = scheduler_id - 1;
    }
    ps = get_fd_ps(handle);
    lock_ps(ps);
    if (read_active)
        ev_io_start(ps->loop, &handle->ev_read);
    if (write_active)
        ev_io_start(ps->loop, &handle->ev_write);
    unlock_ps(ps);

    __sync_add_and_fetch(&ps->socket_online, 1);

    wrunlock_fd(handle);
    ps_notify(ps);
    return old;
}

static ERL_NIF_TERM
nif_bind_pollset(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    int old, pollset;

    PREPARE_GET_HANDLE

    if (!enif_get_int(env, argv[1], &pollset) || pollset < 0 ||
        pollset > ncpus)
        return enif_make_badarg(env);
    /*
     * user code use pollset id, pollset id is 1 based while pollset index
     * is zero-based.
     */
    --pollset; /* convert to index */
    old = bind_pollset(handle, pollset, 0);
    old++;     /* convert to id */
    return enif_make_tuple2(env, TermOk, enif_make_int(env, old));
}

static ERL_NIF_TERM
nif_getstat(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    ERL_NIF_TERM head, tail;

    int idx_array[SOCKET_STAT_ITEMS];
    int idx_cnt = 0;

    tail = argv[1];

    unsigned int len;
    if (enif_get_list_length(env, tail, &len) == 0 || len > SOCKET_STAT_ITEMS)
            return enif_make_badarg(env);

    while(enif_get_list_cell(env, tail, &head, &tail)) {
        if (enif_is_number(env, head)) {
            int tmp;
            if (enif_get_int(env, head, &tmp))
                if (tmp >= 0 && tmp < SOCKET_STAT_ITEMS) {
                    idx_array[idx_cnt++] = tmp;
                    continue;
                }
        }
        return enif_make_badarg(env);
    }

    struct SocketResource *handle;

    PREPARE_GET_HANDLE

    ERL_NIF_TERM arr[SOCKET_STAT_ITEMS];

    int i;
    for (i = 0; i < idx_cnt; i++) {
        int stat_idx = idx_array[i];

        UINT64 v;
        switch(stat_idx) {
        case gen_socket_stat_recv_avg: // Conver double to UINT64
            v = handle->stat.socket_stat.recv_avg;
            break;
        case gen_socket_stat_recv_dvi: // Conver double to UINT64
            v = handle->stat.socket_stat.recv_dvi;
            break;
        case gen_socket_stat_send_avg: // Conver double to UINT64
            v = handle->stat.socket_stat.send_avg;
            break;
        default: // Get from union.v using stat_idx
            v = handle->stat.v[stat_idx];
            break;
        }

        arr[i] = enif_make_tuple2(env,
                                  enif_make_int(env, stat_idx),
                                  enif_make_int64(env, v));
    }

    ERL_NIF_TERM list = enif_make_list_from_array(env, arr, idx_cnt);
    return enif_make_tuple2(env, TermOk, list);
}

/*
 * Just like erlang:port_info()
 *
 * But, the return type is different:
 *     erlang:port_info() return type is erl_term.h::make_small()
 *     nif_socket_info() return type is just a fd type
 */
static ERL_NIF_TERM
nif_socket_info(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    char atom[64];
    int poller_id;
    int fd;
    int ev_is_reading;
    int ev_is_writing;

    PREPARE_GET_FD

    if (!get_atom(env, argv[1], atom, sizeof(atom)))
        return enif_make_badarg(env);

    if (!strcmp(atom, "id")) {
        if (fd<0)
            return TermUndefined;
        return enif_make_tuple2(env, TermId, enif_make_int(env, fd));
    }

    poller_id = handle->pollset + 1;
    if (!strcmp(atom, "bind_id")) {
        return enif_make_tuple2(env, TermSocketBindId, enif_make_int(env, poller_id));
    }

    if (!strcmp(atom, "ev_stat")) {
        ev_is_reading = (int)ev_is_active(&handle->ev_read);
        ev_is_writing = (int)ev_is_active(&handle->ev_write);
        return enif_make_tuple4(env, enif_make_int(env, handle->active_setting),
                                enif_make_int(env, ev_is_reading),
                                enif_make_int(env, ev_is_writing),
                                enif_make_int(env, poller_id));
    }

    return enif_make_badarg(env);
}

static inline void
fill_addr_info(unsigned char *buf, sockaddr_in *addr)
{
    buf[0] = INET_AF_INET;
    short port = ntohs(addr->sin_port);
    buf[1] = (port >>8) & 0xff;
    buf[2] = (port) & 0xff;
    memcpy(buf + 3, (unsigned char*)&addr->sin_addr, sizeof(struct in_addr));
}

/*
 * Return: [INET_AF_INET, port0, port1, ip0, ip1, ip2, ip3]
 */
static ERL_NIF_TERM
nif_peername(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    int fd;

    PREPARE_GET_FD

    if (!handle->peer_ptr) {
        struct sockaddr_in *addr = &handle->peer_info.addr;
        socklen_t len = sizeof(struct sockaddr_in);

        if (getpeername(fd, (struct sockaddr*)addr, &len)<0)
            return make_posix_error(env, errno);

        handle->peer_ptr = &handle->peer_info;
        fill_addr_info(handle->peer_ptr->bytes, addr);
    }

    ErlNifBinary bin;
    if (!enif_alloc_binary(sizeof(handle->peer_ptr->bytes), &bin))
        return make_posix_error(env, ENOMEM);
    memcpy(bin.data, handle->peer_ptr->bytes, sizeof(handle->peer_ptr->bytes));

    return enif_make_tuple2(env, TermOk, enif_make_binary(env, &bin));
}

/*
 * Return: [INET_AF_INET, port0, port1, ip0, ip1, ip2, ip3]
 */
static ERL_NIF_TERM
nif_sockname(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct SocketResource *handle;
    int fd;

    PREPARE_GET_FD

    if (!handle->sock_ptr) {
        struct sockaddr_in *addr = &handle->sock_info.addr;
        socklen_t len = sizeof(struct sockaddr_in);
        if (getsockname(fd, (struct sockaddr*)addr, &len)<0)
            return make_posix_error(env, errno);

        handle->sock_ptr = &handle->sock_info;
        fill_addr_info(handle->sock_ptr->bytes, addr);
    }

    ErlNifBinary bin;

    if (!enif_alloc_binary(sizeof(handle->sock_ptr->bytes), &bin))
        return make_posix_error(env, ENOMEM);
    memcpy(bin.data, handle->sock_ptr->bytes, sizeof(handle->sock_ptr->bytes));

    return enif_make_tuple2(env, TermOk, enif_make_binary(env, &bin));
}

static ERL_NIF_TERM
nif_set_scheduler_id(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int id;

    if (!enif_get_int(env, argv[0], &id))
        return enif_make_badarg(env);
    if (id <= 0 || id > ncpus)
        return make_posix_error(env, EINVAL);
    scheduler_id = id;
    return TermOk;
}

static void
l_release (EV_P)
{
    struct PollSet *ps = (struct PollSet *)ev_userdata (EV_A);
    enif_mutex_unlock(ps->lock);
}

static void
l_acquire (EV_P)
{
    struct PollSet *ps = (struct PollSet *)ev_userdata (EV_A);
    enif_mutex_lock(ps->lock);
}

static void
null_invoke_pending_cb(EV_P)
{
}

static ERL_NIF_TERM
nif_evrun(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    struct PollSet *ps;
    int idx, invoke_maxcnt, invoke_total;

    if (!enif_get_int(env, argv[0], &idx) || idx < 0 || idx >= ncpus)
        return enif_make_badarg(env);
    if (!enif_get_int(env, argv[1], &invoke_maxcnt) || invoke_maxcnt < 0)
        return enif_make_badarg(env);
    invoke_total = 0;
    ps = &pollsets[idx];
    lock_ps(ps);
    if (invoke_maxcnt == 0) {
        if (!(ps->running & EV_WORKING)) {
            ps->running |= EV_WORKING;
            ev_run(ps->loop, EVRUN_NOWAIT);
            ps->running &= ~EV_WORKING;
            if (ps->running & EV_WANT) {
                ps->running &= ~EV_WANT;
                enif_cond_broadcast(ps->cond);
            }
        }
        invoke_total = ev_pending_count(ps->loop);
    	ev_invoke_pending(ps->loop);
    } else {
        int invoked = 0;
        int pending_cnt = ev_pending_count(ps->loop);
        while (invoke_maxcnt > 0) {
            /* first invoke pending events */
            int invoke_cnt = invoke_maxcnt > pending_cnt ? pending_cnt :
                                invoke_maxcnt;
            if (invoke_cnt > 0) {
                invoked = ev_invoke_pending2(ps->loop, invoke_cnt);
                invoke_maxcnt -= invoked;
                invoke_total += invoked;
                pending_cnt = ev_pending_count(ps->loop);
            } else {
                /* query new events */
                if (!(ps->running & EV_WORKING)) {
                    ps->running |= EV_WORKING;
                    ev_run(ps->loop, EVRUN_NOWAIT);
                    ps->running &= ~EV_WORKING;
                    if (ps->running & EV_WANT) {
                        ps->running &= ~EV_WANT;
                        enif_cond_broadcast(ps->cond);
                    }
                }
                pending_cnt = ev_pending_count(ps->loop);
                if (pending_cnt == 0)
                    break;
            }
        }
    }
    unlock_ps(ps);
    return enif_make_tuple2(env, TermOk, enif_make_int(env, invoke_total));
}

static void *psmon_main(void *arg)
{
    struct PollSet *ps = (struct PollSet *)arg;
    struct PSMonResource *handle;

    lock_ps(ps);
    for (;;) {
again:
        /*
         * Check if there is any erlang process wants to monitor the pollset,
         * if not have, just wait, don't bother to check it.
         */
        while (TAILQ_EMPTY(&ps->mon_list) && !ps->stop)
            enif_cond_wait(ps->cond, ps->lock);
        if (ps->stop)
            break;
        /*
         * Check if there is anyone checking the pollset, if yes, just wait,
         * ev_run can not be re-entered.
         */
        if (ps->running & EV_WORKING) {
            while (ps->running & EV_WORKING) {
                ps->running |= EV_WANT;
                enif_cond_wait(ps->cond, ps->lock);
            }
            /* we should retry because there may be no waiters */
            goto again;
        }

        /* EV_AUX means the thread is executing ev_run */
        ps->running |= EV_WORKING | EV_AUX;
        ev_run(ps->loop, EVRUN_ONCE);
        ps->running &= ~(EV_WORKING | EV_AUX);
        if (ev_pending_count(ps->loop)) {
            /* notify any erlang process */
            while (!TAILQ_EMPTY(&ps->mon_list)) {
                handle = TAILQ_FIRST(&ps->mon_list);
                assert(handle->on_list);
                remove_mon_list(ps, handle);
                send_ps_mon(handle, ps);
            }
        }

        /* Anyone blocking ? */
        if (ps->running & EV_WANT) {
            ps->running &= ~EV_WANT;
            enif_cond_broadcast(ps->cond);
        }
    }
    unlock_ps(ps);
    DBG("psmain exit\n");
    return 0;
}

static void async_cb (EV_P_ ev_async *w, int revents)
{
    struct PollSet *ps = (struct PollSet *)ev_userdata (EV_A);

    if (ps->stop)
        ev_break (EV_A_ EVBREAK_ALL);
}

static void init_pollset(struct PollSet *ps)
{
    ps->loop = ev_loop_new(0);
    ps->lock = enif_mutex_create(const_cast<char *>("evloop"));
    ps->cond = enif_cond_create(const_cast<char *>("evcond"));
    ps->stop = 0;
    ps->running = 0;
    ps->msg_lock = enif_mutex_create(const_cast<char *>("evmsg"));
    ps->msg_env = enif_alloc_env();
    ev_async_init(&ps->async_w, async_cb);
    ev_async_start(ps->loop, &ps->async_w);
    ev_set_userdata(ps->loop, ps);
    ev_set_loop_release_cb(ps->loop, l_release, l_acquire);
    ev_set_invoke_pending_cb(ps->loop, null_invoke_pending_cb);
    TAILQ_INIT(&ps->mon_list);
    enif_thread_create((char *)"psmon", &ps->aux_thread, psmon_main,
        ps, NULL);
}

static void destroy_pollset(struct PollSet *ps)
{
    void *result;

    lock_ps(ps);
    ps->stop = 1;
    enif_cond_broadcast(ps->cond);
    if (ps->running & EV_AUX) {
        ps_notify(ps);
    }
    unlock_ps(ps);
    enif_thread_join(ps->aux_thread,  &result);
    ev_loop_destroy(ps->loop);
    enif_cond_destroy(ps->cond);
    enif_mutex_destroy(ps->lock);
    enif_mutex_destroy(ps->msg_lock);
    enif_free_env(ps->msg_env);
}

static int on_load(ErlNifEnv* env, void** priv_data, ERL_NIF_TERM load_info)
{
    ErlNifSysInfo si;
    int i;

    ErlNifResourceFlags flags;

    flags = (ErlNifResourceFlags)(ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER);
    ResTypeSock = enif_open_resource_type(env, "raw_socket", "Socket",
        file_dtor, flags, NULL);
    ResTypeBuff = enif_open_resource_type(env, "raw_socket", "SockBuffer",
        buff_dtor, flags, NULL);
    ResTypePSMon = enif_open_resource_type(env, "raw_socket", "PSMon",
        psmon_dtor, flags, NULL);

    TermOk = enif_make_atom(env, "ok");
    TermError = enif_make_atom(env, "error");
    TermClosed = enif_make_atom(env, "closed");
    TermSocketEv = enif_make_atom(env, "socket_event");
    TermRead = enif_make_atom(env, "readable");
    TermWrite = enif_make_atom(env, "writable");
    TermData = enif_make_atom(env, "data");
    TermPassive = enif_make_atom(env, "tcp_passive");
    TermPSEv = enif_make_atom(env, "psev");
    TermUndefined = enif_make_atom(env, "undefined");
    TermId = enif_make_atom(env, "id");
    TermTcp = enif_make_atom(env, "tcp");
    TermTcpClosed = enif_make_atom(env, "tcp_closed");
    TermTcpError = enif_make_atom(env, "tcp_error");

    TermSocketBindId = enif_make_atom(env, "bind_id");
    TermReadActive = enif_make_atom(env, "read_active");
    TermWriteActive = enif_make_atom(env, "write_active");
    TermActiveN = enif_make_atom(env, "active_n");
    TermActiveSetting = enif_make_atom(env, "active_setting");
    TermFD = enif_make_atom(env, "fd");
    TermBindPolicy = enif_make_atom(env, "bind_policy");
    TermSocketOnline = enif_make_atom(env, "socket_online");
    TermBinaryListSize = enif_make_atom(env, "binary_list_size");

    bind_policy = BIND_POLICY_RR;

    next_pollset_lock = enif_mutex_create(const_cast<char *>("next_pollset"));
    enif_system_info(&si, sizeof(si));
    ncpus = si.scheduler_threads;
    pollsets = (struct PollSet *)valloc(ncpus * sizeof(struct PollSet));
    bzero(pollsets, ncpus * sizeof(struct PollSet));
    for (i = 0; i < ncpus; ++i)
        init_pollset(&pollsets[i]);
    return (0);
}

static void on_unload(ErlNifEnv* env, void* priv_data)
{
    int i;

    for (i = 0; i < ncpus; ++i)
        destroy_pollset(&pollsets[i]);
    free(pollsets);
    enif_mutex_destroy(next_pollset_lock);
}

static ErlNifFunc nif_funcs[] =
{
    {"socket", 0, nif_socket, 0},
    {"bind", 3, nif_bind, 0},
    {"listen", 2, nif_listen, 0},
    {"close", 1, nif_close, 0},
    {"enable_events", 3, nif_enable_events, 0},
    {"disable_events", 2, nif_disable_events, 0},
    {"accept", 1, nif_accept, 0},
    {"connect", 3, nif_connect, 0},
    {"setsockopt", 2, nif_setsockopt, 0},
    {"getsockopt", 2, nif_getsockopt, 0},
    {"recv", 1, nif_recv, 0},
    {"send", 4, nif_send, 0},
    {"shutdown", 2, nif_shutdown, 0},
    {"controlling_process", 2, nif_controlling_process, 0},
    {"alloc_buf", 2, nif_alloc_buf, 0},
    {"realloc_buf", 3, nif_realloc_buf, 0},
    {"free_buf", 1, nif_free_buf, 0},
    {"get_buf_info", 1, nif_get_buf_info, 0},
    {"get_buf_data", 3, nif_get_buf_data, 0},
    {"recv_buf", 2, nif_recv_buf, 0},
    {"convert_buf_to_binary", 1, nif_convert_buf_to_binary, 0},
    {"alloc_psmon", 1, nif_alloc_psmon, 0},
    {"enable_psmon", 1, nif_enable_psmon, 0},
    {"disable_psmon", 1, nif_disable_psmon, 0},
    {"total_pollsets", 0, nif_total_pollsets, 0},
    {"evrun", 2, nif_evrun, 0},
    {"bind_pollset", 2, nif_bind_pollset, 0},
    {"getstat", 2, nif_getstat, 0},
    {"socket_info", 2, nif_socket_info, 0},
    {"peername", 1, nif_peername, 0},
    {"sockname", 1, nif_sockname, 0},
    {"set_scheduler_id", 1, nif_set_scheduler_id, 0},
    {"dup_socket", 1, nif_dup_socket, 0},
    {"sendv", 3, nif_sendv, 0},
    {"set_bind_policy", 1, nif_set_bind_policy, 0},
    {"ev_info", 1, nif_ev_info, 0},
    {"socketonline_info", 0, nif_socketonline_info, 0},
    {"iolist_to_binary_list", 1, nif_iolist_to_binary_list, 0},


};

ERL_NIF_INIT(raw_socket, nif_funcs, on_load, NULL, NULL, on_unload)
