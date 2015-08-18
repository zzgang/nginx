/*
 * Copyright (C) Zhigang Zhang
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_md5.h>
#include <sys/stat.h>

#define DEFAULT_FEED_LOG_ROOT "feed"
#define DEFAULT_FEED_LOG_FNAME "feed"
#define FEED_LOG_FILE_LOCK "feed_lock"
#define FEED_LOG_FNAME_UNIQ_DUMMY_STR "XXXXXX"
#define FEED_LOG_DATE_PADDING_STR "YYYY_MM_DD_hrHH"
#define FEED_LOG_DATE_FMT_STR "%4d_%02d_%02d_hr%02d"
#define FEED_LOG_DATE_NUM_FMT(date_num) (date_num < 10 ? "0%d" : "%d")
#define FEED_LOG_FNAME_PADDING_TEMPLATE "_"FEED_LOG_DATE_PADDING_STR"_"FEED_LOG_FNAME_UNIQ_DUMMY_STR".log"
#define LOG_HOST_DIR 1
#define MAX_FEED_LOG_FILE_NUM (1<<10)
#define MAX_FEED_LOG_FILE_SIZE (1<<26)
#define MAX_FEED_LOG_FNAME_LEN 256
#define FEED_LOG_UNLIKED_FILE -1
#define MD5_BIN_LEN 16
#define MD5_STR_LEN 32
#define FEED_LOG_LINE_BUF_SIZE 4096
#define FEED_LOG_DIR_DEFAULT_ACCESS 0755

#define DEFAULT_FEED_LOG_FORMAT "$remote_addr - [$local_time] $host \"$request\" $args $status $cookies \"$referer\" \"$user_agent\""

#define FEED_LOG_LINE_VAR_ENTRY(var_name) \
    {ngx_string(#var_name), {-1, NULL}, feed_log_line_var_get_##var_name, {-1, NULL}, -1, 0}

#define FEED_LOG_LINE_VAR_ARG_ENTRY(var_name, var_param) \
    {ngx_string(#var_name), ngx_string(var_param), feed_log_line_var_get_from_args, {-1, NULL}, -1, 0}

#define FEED_LOG_LINE_VAR_NULL \
    {{-1, NULL}, {-1, NULL}, NULL, {-1, NULL}, -1, 0}

#define FEED_LOG_LINE_VAR_GET(var_name) \
static ngx_int_t feed_log_line_var_get_##var_name(ngx_str_t *param, \
        ngx_str_t *value, \
        ngx_http_request_t *r)

#define do_feed_log_get_uniq_fname  mktemp

#define FEED_LOG_HASH_FIND(head, fname)                                       \
    ({                                                                        \
        feed_log_file_t *__p;                                                 \
        for (__p = head; __p != NULL; __p = __p->hash_next)                   \
            if (ngx_strncmp(__p->ffname.data, (fname)->data, (fname)->len) == 0)  \
                break;                                                        \
        __p;                                                                  \
    })

#define FEED_LOG_HASH_ADD(head, bucket)     \
    do {                                    \
        (bucket)->hash_next = head;         \
        (bucket)->hash_prev = NULL;         \
        if (head != NULL) {                 \
            (head)->hash_prev = bucket;     \
        }                                   \
        head = bucket;                      \
    } while(0)

#define FEED_LOG_HASH_REMOVE(head, bucket)                          \
    do {                                                            \
        if ((bucket)->hash_prev != NULL) {                          \
            (bucket)->hash_prev->hash_next = (bucket)->hash_next;   \
        }                                                           \
        if ((bucket)->hash_next != NULL) {                          \
            (bucket)->hash_next->hash_prev = (bucket)->hash_prev;   \
        }                                                           \
        if (head == bucket) {                                       \
            head = bucket->hash_next;                               \
        }                                                           \
    } while(0)


#define FEED_LOG_PATH_EXISTS(pathname)                  \
    ({                                                  \
        ngx_int_t __ret;                                \
        __ret = access((const char *)(pathname), F_OK); \
        __ret == -1 ? 0 : 1;                            \
     })                                         

typedef struct {
    u_char data[MAX_FEED_LOG_FNAME_LEN + 1]; //padding '\0'
    size_t len;
} feed_fstr_t;

typedef struct {
    u_char data[sizeof(FEED_LOG_DATE_PADDING_STR)];
    size_t len;
} feed_tstr_t;

typedef struct feed_log_file_s{
    feed_fstr_t ffname;   //full file name.
    uint64_t file_size;
    uint64_t write_len;
    feed_tstr_t create_date;
    ngx_int_t in_use;

    struct feed_log_file_s *free_next;

    struct feed_log_file_s *hash_prev;
    struct feed_log_file_s *hash_next;

} feed_log_file_t;

typedef struct {
    ngx_fd_t fd;
    feed_fstr_t *ffp;
} feed_log_file_ex_t;


typedef struct {
    ngx_shmtx_sh_t shmtx_addr;
    feed_log_file_t *hash_table[MAX_FEED_LOG_FILE_NUM];
    feed_log_file_t *free_list;
    feed_log_file_t feed_log_files[MAX_FEED_LOG_FILE_NUM];
} feed_log_file_summary_shm_t;

typedef struct {
    ngx_shm_t shm;
    ngx_shmtx_t shmtx;
    feed_log_file_summary_shm_t *addr;
} feed_log_shm_t;

typedef struct {
    ngx_str_t name;
    ngx_str_t param;
    ngx_int_t (*get_val)(ngx_str_t *, ngx_str_t *, ngx_http_request_t *);
    ngx_str_t value;
    ngx_int_t pos;
    ngx_int_t valid;
} feed_log_line_var_t;

typedef struct {
    ngx_str_t buf;
    size_t size; //preallocate mem size.
} feed_log_line_t;

typedef struct {
    ngx_uint_t log_feed;
    ngx_str_t log_format;
    ngx_str_t log_root;
    ngx_str_t log_root_dir;
} ngx_http_log_feed_srv_conf_t;

static void* ngx_http_log_feed_create_srv_conf(ngx_conf_t *cf);
static char* ngx_http_log_feed_merge_srv_conf(ngx_conf_t *cf, 
        void *parent, void *child);

static ngx_int_t ngx_http_log_feed_init(ngx_conf_t *cf);

static ngx_int_t do_log_feed(ngx_str_t *dir, ngx_str_t *fname, 
        ngx_str_t *log_format, ngx_http_request_t *r);

static ngx_int_t ngx_http_log_feed_module_init(ngx_cycle_t *cycle);
static void ngx_http_log_feed_module_exit(ngx_cycle_t *cycle);

static ngx_int_t feed_log_create_newfile(ngx_str_t *fname, 
        feed_log_file_t *feed_log_p, 
        ngx_http_request_t *r);
static ngx_int_t feed_log_get_file(feed_log_file_ex_t *fp,
        ngx_int_t write_len,
        ngx_str_t *fname, 
        feed_log_shm_t *shm, 
        ngx_http_request_t *r);

static ngx_int_t feed_log_get_id(u_char *feed_id, ngx_http_request_t *r);

FEED_LOG_LINE_VAR_GET(remote_addr);
FEED_LOG_LINE_VAR_GET(local_time);
FEED_LOG_LINE_VAR_GET(status);
FEED_LOG_LINE_VAR_GET(cookies);
FEED_LOG_LINE_VAR_GET(user_agent);
FEED_LOG_LINE_VAR_GET(args);
static ngx_int_t feed_log_line_var_get_from_args(ngx_str_t *param, ngx_str_t *value, 
        ngx_http_request_t *r);


static ngx_str_t feed_gif = ngx_string("/feed.gif");

static feed_log_shm_t feed_log_shm = {
    .shm = {
        .addr = NULL,
        .size = sizeof(feed_log_file_summary_shm_t)
    }
};

static feed_log_line_var_t feed_log_line_vars[] = {
    FEED_LOG_LINE_VAR_ENTRY(remote_addr),
    FEED_LOG_LINE_VAR_ENTRY(local_time),
    FEED_LOG_LINE_VAR_ENTRY(status),
    FEED_LOG_LINE_VAR_ENTRY(cookies),
    FEED_LOG_LINE_VAR_ENTRY(user_agent),
    FEED_LOG_LINE_VAR_ARG_ENTRY(host, "WF.host"),
    FEED_LOG_LINE_VAR_ARG_ENTRY(request, "WF.request"),
    FEED_LOG_LINE_VAR_ARG_ENTRY(referer, "WF.referer"), 
    FEED_LOG_LINE_VAR_ENTRY(args), //must be last
    FEED_LOG_LINE_VAR_NULL
};

static feed_log_line_var_t *feed_log_line_vps[sizeof(feed_log_line_vars)] = {NULL, };

static struct {
     ngx_fd_t fd;                   //open fd in current process.
     feed_fstr_t ffname;            //opened full file name.
} cached_files[MAX_FEED_LOG_FILE_NUM];

static ngx_command_t  ngx_http_log_feed_commands[] = {
    { 
        ngx_string("log_feed"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_flag_slot,
        NGX_HTTP_SRV_CONF_OFFSET,
        offsetof(ngx_http_log_feed_srv_conf_t, log_feed),
        NULL 
    },
    { 
        ngx_string("feed_log_root"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_SRV_CONF_OFFSET,
        offsetof(ngx_http_log_feed_srv_conf_t, log_root),
        NULL 
    },
    { 
        ngx_string("feed_log_format"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_SRV_CONF_OFFSET,
        offsetof(ngx_http_log_feed_srv_conf_t, log_format),
        NULL 
    },
    ngx_null_command
};

static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;

static ngx_http_module_t  ngx_http_log_feed_module_ctx = {
    NULL,                               /* preconfiguration */
    ngx_http_log_feed_init,             /* postconfiguration */

    NULL,                               /* create main configuration */
    NULL,                               /* init main configuration */

    ngx_http_log_feed_create_srv_conf,  /* create server configuration */
    ngx_http_log_feed_merge_srv_conf,   /* merge server configuration */

    NULL,                               /* create location configuration */
    NULL                                /* merge location configuration */
};

ngx_module_t  ngx_http_log_feed_module = {
    NGX_MODULE_V1,
    &ngx_http_log_feed_module_ctx,     /* module context */
    ngx_http_log_feed_commands,        /* module directives */
    NGX_HTTP_MODULE,                   /* module type */
    NULL,                              /* init master */
    ngx_http_log_feed_module_init,     /* init module */
    NULL,                              /* init process */
    NULL,                              /* init thread */
    NULL,                              /* exit thread */
    NULL,                              /* exit process */
    ngx_http_log_feed_module_exit,     /* exit master */
    NGX_MODULE_V1_PADDING
};

static inline ngx_uint_t _hashfn(ngx_str_t *str)
{
    ngx_uint_t i, hash = 0;

    for (i= 0;i < str->len; i++) {
        hash = (hash << 5) + (ngx_uint_t)str->data[i];
    }

    return hash;
}

static inline void feed_log_get_now_date(feed_tstr_t *date_str)
{
    ngx_tm_t tm;
    time_t now;

    now = ngx_time();
    ngx_localtime(now, &tm);

    date_str->len = ngx_sprintf(date_str->data, 
            FEED_LOG_DATE_FMT_STR, 
            tm.ngx_tm_year, tm.ngx_tm_mon, tm.ngx_tm_mday, tm.ngx_tm_hour, tm.ngx_tm_min, tm.ngx_tm_sec) - date_str->data;
}

static inline ngx_int_t feed_log_check_file(feed_log_file_t *feed_log_p, 
        ngx_http_request_t *r)
{
    feed_tstr_t now_date;
    /*
    ngx_file_info_t fi; 
    
    if (ngx_file_info(feed_log_p->ffname.data, &fi) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno, 
                "Stat %s failed\n", 
                feed_log_p->ffname.data);
        return FEED_LOG_UNLIKED_FILE;
    }

    if (fi.st_size >= MAX_FEED_LOG_FILE_SIZE) {
        return FEED_LOG_UNLIKED_FILE;
    }
    */

    feed_log_get_now_date(&now_date);

    if (ngx_strncmp(now_date.data, feed_log_p->create_date.data, 
                now_date.len) != 0) {
        return FEED_LOG_UNLIKED_FILE;
    }

    if ((feed_log_p->file_size + feed_log_p->write_len) > MAX_FEED_LOG_FILE_SIZE) {
        return FEED_LOG_UNLIKED_FILE;
    }

    return NGX_OK;
}

static inline void feed_log_get_uniq_fname(ngx_str_t *uniq_str)
{
    uniq_str->data = (u_char *)do_feed_log_get_uniq_fname((char *)uniq_str->data);
}

static inline void ngx_md5_get(u_char *md5_str_buf, u_char *data, size_t len)
{
    ngx_int_t i;
    ngx_md5_t md5;
    u_char md5_bin_buf[MD5_BIN_LEN];

    ngx_md5_init(&md5);
    ngx_md5_update(&md5, data, len);
    ngx_md5_final(md5_bin_buf, &md5);

    for (i = 0; i < MD5_STR_LEN; i++) {
        ngx_int_t idx = i/2;
        if (i%2 == 0) {
            md5_str_buf[i] = md5_bin_buf[idx] & 0xF0;
            md5_str_buf[i] >>= 4;
        } else {
            md5_str_buf[i] = md5_bin_buf[idx] & 0x0F;
        }
        
        if (md5_str_buf[i] <= 9) {
            md5_str_buf[i] += '0'; 
            continue;
        }
        
        if (md5_str_buf[i] >= 10) {
            md5_str_buf[i] = (md5_str_buf[i] - 10) + 'A';
        }
    }
}

static inline ngx_int_t feed_log_write_file(feed_log_file_ex_t *fp, 
        u_char *buf, ngx_int_t count, 
        ngx_http_request_t *r)
{
    ngx_int_t ret;

    do {
        ret = write(fp->fd, (const char *)buf, count);
    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

    if (ret == -1) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno, 
                "Write file %s error: %s!", fp->ffp->data);
        if (ngx_close_file(fp->fd) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno, 
                    "Close file %s failed!", fp->ffp->data);
        }
        return NGX_ERROR;
    } 

    return NGX_OK;
}

static ngx_int_t feed_log_create_newfile(ngx_str_t *fname, 
        feed_log_file_t *feed_log_p, 
        ngx_http_request_t *r)
{
    ngx_fd_t fd;
    ngx_str_t uniq_str;
    feed_tstr_t date_str = {
        .data = {0, }, 
        .len = sizeof(FEED_LOG_DATE_PADDING_STR) - 1
    };
    ngx_str_t feed_log_init_msg = ngx_string(
"#Software: FeedLog SmartSource Data Collector\n"
"#Version: 1.0\n"
);

    ngx_int_t retry = 0;

    if (fname->len + sizeof(FEED_LOG_FNAME_PADDING_TEMPLATE) -1 
            > MAX_FEED_LOG_FNAME_LEN) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, 
                "Log file name exceeds the max name len limit: %d!",
                MAX_FEED_LOG_FNAME_LEN-sizeof(FEED_LOG_FNAME_PADDING_TEMPLATE)+1);
        return NGX_ERROR;
    }
    
    uniq_str.len = sizeof(FEED_LOG_FNAME_UNIQ_DUMMY_STR) - 1;
    uniq_str.data = ngx_pcalloc(r->pool, uniq_str.len + 1);
    if (uniq_str.data == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory!");
        return NGX_ERROR;
    }
    ngx_memcpy(uniq_str.data, FEED_LOG_FNAME_UNIQ_DUMMY_STR, 
            sizeof(FEED_LOG_FNAME_UNIQ_DUMMY_STR) - 1);

repeat: 
    feed_log_get_now_date(&date_str);
    
    feed_log_get_uniq_fname(&uniq_str);
    
    ngx_memzero(&feed_log_p->ffname, sizeof(feed_fstr_t));
    feed_log_p->ffname.len = ngx_sprintf(feed_log_p->ffname.data, 
            "%s_%s_%s.log", 
            fname->data, date_str.data, uniq_str.data) - feed_log_p->ffname.data;

    fd = ngx_open_file(feed_log_p->ffname.data, 
            O_WRONLY, O_CREAT|O_EXCL, 
            NGX_FILE_DEFAULT_ACCESS);

    if (fd == NGX_INVALID_FILE) {
        if (ngx_errno != EEXIST) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                    ngx_open_file_n " \"%s\" failed!", feed_log_p->ffname.data);
            return NGX_ERROR;
        }
        if (retry++ < 3) {
            goto repeat;
        } else {
            return NGX_ERROR;
        }
    }
   
    {
        feed_log_file_ex_t flf;
        flf.fd = fd;
        flf.ffp = &feed_log_p->ffname;
        if (feed_log_write_file(&flf, feed_log_init_msg.data, feed_log_init_msg.len, r)
                != NGX_OK) {
            return NGX_ERROR;
        }
    }

    feed_log_p->file_size = feed_log_init_msg.len; 

    if (ngx_close_file(fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno, 
                "Close %s failed!", 
                feed_log_p->ffname.data);
    }

    feed_log_p->create_date.len = date_str.len;
    ngx_memcpy(feed_log_p->create_date.data, date_str.data, date_str.len);

    return NGX_OK;
}

static ngx_int_t feed_log_get_file(feed_log_file_ex_t *fp, 
        ngx_int_t write_len,
        ngx_str_t *fname, 
        feed_log_shm_t *shm, 
        ngx_http_request_t *r)
{
    feed_log_file_t *feed_log_p;
    feed_log_file_summary_shm_t *FLG = shm->addr;
    ngx_uint_t hash = _hashfn(fname) % MAX_FEED_LOG_FILE_NUM;
    ngx_int_t current_cached_file_idx;
    typeof(cached_files[0]) *current_cached_file;

    ngx_shmtx_lock(&shm->shmtx);

    feed_log_p = FEED_LOG_HASH_FIND(FLG->hash_table[hash], fname);
    if (feed_log_p != NULL) {
        feed_log_p->write_len = write_len;
        if (feed_log_check_file(feed_log_p, r) == FEED_LOG_UNLIKED_FILE) {
            feed_log_p->in_use = 0;  
            FLG->free_list = feed_log_p;
            FEED_LOG_HASH_REMOVE(FLG->hash_table[hash], feed_log_p);
            goto create_newfile;
        } else {
            goto found;
        }
    }

create_newfile:
    //get free feed log file.
    feed_log_p = FLG->free_list; 
    while (feed_log_p->free_next != FLG->free_list) {
        if (feed_log_p->in_use == 0) {
            break;
        }
        feed_log_p = feed_log_p->free_next;
    }

    if (feed_log_p->in_use != 0) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, 
                "Serving feed log files exceeds max feed files limit: %d!", 
                MAX_FEED_LOG_FILE_NUM);
        goto out_unlock_err;
    }

    if (feed_log_create_newfile(fname, feed_log_p, r) == NGX_OK) {
        FEED_LOG_HASH_ADD(FLG->hash_table[hash], feed_log_p);
        feed_log_p->in_use = 1;
    } else {
        goto out_unlock_err;
    }

found:
    feed_log_p->file_size += write_len;
    current_cached_file_idx = ((u_char *)feed_log_p - (u_char *)FLG->feed_log_files)/(sizeof(*feed_log_p));
   current_cached_file = cached_files + current_cached_file_idx;

    if (current_cached_file->fd >= 0) {
        if ((current_cached_file->ffname.len == feed_log_p->ffname.len) && ngx_strncmp(current_cached_file->ffname.data, feed_log_p->ffname.data, current_cached_file->ffname.len) == 0) {
            goto out_cached;
        } else {
            if (ngx_close_file(current_cached_file->fd) == NGX_FILE_ERROR) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno, 
                        "Close %s failed!", 
                        current_cached_file->ffname.data);
            }
        }
    }

    current_cached_file->fd = ngx_open_file(feed_log_p->ffname.data, 
            O_WRONLY, O_APPEND, 
            NGX_FILE_DEFAULT_ACCESS);

    if (current_cached_file->fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                ngx_open_file_n " \"%s\" failed!", feed_log_p->ffname.data);
        goto out_unlock_err;
    } else {
        current_cached_file->ffname.len = feed_log_p->ffname.len;
        ngx_memcpy(current_cached_file->ffname.data, feed_log_p->ffname.data, feed_log_p->ffname.len); 
    }

out_cached:
    ngx_shmtx_unlock(&shm->shmtx);
    fp->fd = current_cached_file->fd;
    fp->ffp = &feed_log_p->ffname;
    return NGX_OK;

out_unlock_err:
    ngx_shmtx_unlock(&shm->shmtx);
    return NGX_ERROR;
}

static void feed_log_sshm_init(feed_log_file_summary_shm_t *sshm)
{
    ngx_int_t i;

    for (i = 0;i < MAX_FEED_LOG_FILE_NUM - 1; i++) {
        sshm->feed_log_files[i].in_use = 0;
        sshm->feed_log_files[i].free_next = &sshm->feed_log_files[i+1];
    }
    sshm->feed_log_files[i].in_use = 0;
    sshm->feed_log_files[i].free_next = sshm->feed_log_files;

    sshm->free_list = sshm->feed_log_files;
}

static inline void feed_log_shm_destroy(feed_log_shm_t *fl_shm) 
{
    ngx_shmtx_destroy(&fl_shm->shmtx);
    ngx_shm_free(&fl_shm->shm);
}


static ngx_int_t feed_log_shm_init(feed_log_shm_t *fl_shm)
{
    if (fl_shm->addr != NULL) {
        feed_log_shm_destroy(fl_shm);
    }

    if (ngx_shm_alloc(&fl_shm->shm) != NGX_OK) {
        return NGX_ERROR;
    }


    fl_shm->addr = (feed_log_file_summary_shm_t *) fl_shm->shm.addr;
    ngx_memzero(fl_shm->addr, fl_shm->shm.size);
    if (ngx_shmtx_create(&fl_shm->shmtx, &fl_shm->addr->shmtx_addr,
                (u_char *)FEED_LOG_FILE_LOCK) != NGX_OK) {

        return NGX_ERROR;
    }
    
    feed_log_sshm_init(fl_shm->addr);

    return NGX_OK;
}

static ngx_int_t ngx_http_log_feed_module_init(ngx_cycle_t *cycle)
{
    if (feed_log_shm_init(&feed_log_shm) != NGX_OK) { 
        return NGX_ERROR;
    }

    //init cached files by master to each process
    {
        ngx_int_t i;
        for(i = 0; i < MAX_FEED_LOG_FILE_NUM; i++) {
            cached_files[i].fd = -1;
        }
    }

    return NGX_OK;
}

static void ngx_http_log_feed_module_exit(ngx_cycle_t *cycle)
{
    feed_log_shm_destroy(&feed_log_shm);
}

static ngx_int_t feed_log_get_id(u_char *feed_id, ngx_http_request_t *r)
{
    ngx_uint_t nip;
    unsigned short int nport;
    struct timeval tv;
    ngx_uint_t rand;
    struct sockaddr_in *sin;
    ngx_str_t uniq_str;
    u_char uniq_str_fmt[] = "%ud:%ud_%ul:%ul_%ud";

    sin = (struct sockaddr_in *)r->connection->sockaddr;
    nip = sin->sin_addr.s_addr;
    nport = sin->sin_port;

    ngx_gettimeofday(&tv);

    rand = ngx_random();

    uniq_str.data = ngx_pcalloc(r->pool, 128);
    if (uniq_str.data == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory!");
        return NGX_ERROR;
    }
    uniq_str.len = ngx_sprintf(uniq_str.data, (const char *)uniq_str_fmt, 
            nip, nport, tv.tv_sec, tv.tv_usec, rand) - uniq_str.data;

    ngx_md5_get(feed_id, uniq_str.data, uniq_str.len);

    return NGX_OK;
}

FEED_LOG_LINE_VAR_GET(remote_addr)
{
    value->data = r->connection->addr_text.data;
    value->len = r->connection->addr_text.len;
    return NGX_OK;
}

FEED_LOG_LINE_VAR_GET(local_time)
{
    ngx_tm_t tm;
    time_t now;
    u_char time_format[] = "yyyy/mm/dd hh:ii:ss";

    now = ngx_time();
    ngx_localtime(now, &tm);

    value->len = sizeof(time_format) - 1;
    value->data = ngx_pcalloc(r->pool, value->len + 1); //prespace for '\0'
    if (value->data == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory!");
        return NGX_ERROR;
    }

    ngx_sprintf(value->data,
            "%4d/%02d/%02d %02d:%02d:%02d",
            tm.ngx_tm_year, tm.ngx_tm_mon, tm.ngx_tm_mday, 
            tm.ngx_tm_hour, tm.ngx_tm_min, tm.ngx_tm_sec);

    return NGX_OK;
}

#define ngx_http_parse_arg(r, arg, len, value) \
    ngx_http_arg(r, (u_char *)arg, len, value)

static ngx_int_t feed_log_line_var_get_from_args(ngx_str_t *param, ngx_str_t *value, 
        ngx_http_request_t * r) 
{
    if (param->data == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Wrong arguments!");
        return NGX_ERROR;
    }

    if (ngx_http_parse_arg(r, param->data, param->len, value) != NGX_OK) {
        value->data = (u_char *)"-";
        value->len = 1;
    } else {
        //strip the vars str from original args
        u_char *new_data, *var_strpos;
        ngx_int_t var_strlen, shift_strlen;
        
        new_data = ngx_pcalloc(r->pool, value->len + 1); //prespace for '\0'
        if (new_data == NULL) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory!");
            return NGX_ERROR;
        }
        ngx_memcpy(new_data, value->data, value->len); //store value->data
       
        var_strpos = value->data - 1/*=*/ - param->len - 1 /*&*/;
        var_strlen = param->len + 1 /*=*/ + value->len + 1 /*&*/;

        shift_strlen = (r->args.data + r->args.len) - (value->data + value->len);

        if (shift_strlen > 0) {
            ngx_memcpy(var_strpos, var_strpos + var_strlen, shift_strlen);
        }
        r->args.len -= var_strlen;

        value->data = new_data;
    } 

    return NGX_OK;
}

FEED_LOG_LINE_VAR_GET(args)
{
    if (r->args.len == 0) {
        value->data = (u_char *)"-";
        value->len = 1;
    } else {
        value->data = r->args.data;
        value->len = r->args.len;
    }

    return NGX_OK;
}

FEED_LOG_LINE_VAR_GET(status)
{
    value->len = 3;
    value->data = ngx_pcalloc(r->pool, value->len + 1);
    if (value->data == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory!");
        return NGX_ERROR;
    }
    sprintf((char *)value->data, "%d", (int)r->headers_out.status);

    return NGX_OK;
}

FEED_LOG_LINE_VAR_GET(cookies)
{
    size_t len;
    u_char *p, *end;
    ngx_uint_t i, n;
    ngx_array_t *a;
    ngx_table_elt_t **h;
    
    a = &r->headers_in.cookies;

    n = a->nelts;
    h = a->elts;

    len = 0;

    for (i = 0; i < n; i++) {

        if (h[i]->hash == 0) {
            continue;
        }

        len += h[i]->value.len + 2;
    }

    if (len == 0) {
        value->data = NULL;
        value->len = 0;
        goto out_parse;
    }

    len -= 2; 

    if (n == 1) {
        value->len = (*h)->value.len;
        value->data = (*h)->value.data;
        
        goto out_parse;
    }

    p = ngx_pcalloc(r->pool, len + 1);
    if (p == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory!");
        return NGX_ERROR;
    }

    value->len = len;
    value->data = p;

    end = p + len;
    for (i = 0; /* void */ ; i++) {

        if (h[i]->hash == 0) {
            continue;
        }

        p = ngx_copy(p, h[i]->value.data, h[i]->value.len);

        if (p == end) {
            break;
        }

        *p++ = ';', *p++ = ' ';
    }

out_parse:
    {
        u_char feed_id_part[] = "+FEED_ID=";
        u_char feed_id[MD5_STR_LEN+1] = {0, };
        ngx_int_t feed_id_found = 0;

        if (value->len > 0) {
            u_char *p;
            p = ngx_pcalloc(r->pool, 1 + value->len); //prepend the '+'
            if (p == NULL) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory!");
                return NGX_ERROR;
            }
            *p++ = '+';
            p = ngx_cpymem(p, value->data, value->len);
            value->data = p - (1 + value->len);
            value->len += 1;
        }

        for (i = 0; i < value->len; i++) {

            if (value->data[i] == ';' && value->data[i+1] == ' ') {
                value->data[i+1] = '+'; 
            }

            if (!feed_id_found 
                    && ngx_strncmp(feed_id_part, &value->data[i], 
                        sizeof(feed_id_part) - 1) == 0) {
                    feed_id_found = 1;
            }

        }

        if (!feed_id_found) {
            u_char *p, *q, *cookie;
            ngx_int_t len;
            u_char expires[] = "; expires=Thu, 31-Dec-37 23:55:55 GMT";
            u_char cookie_path[] = "; path=/";
            ngx_table_elt_t *set_cookie;
           
            //prepend feed_id cookie 
            if (feed_log_get_id(feed_id, r) != NGX_OK) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, 
                        "Get feed id error!");
                return NGX_ERROR;
            }

            len = (sizeof(feed_id_part) - 1) + MD5_STR_LEN;
            if (n > 0) {
                len +=  1 + value->len; //prespace for ';'
            }
            p = ngx_pcalloc(r->pool, len + 1); //prespace for '\0'
            if (p == NULL) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory!");
                return NGX_ERROR;
            }
            q = p; 
            p = ngx_cpymem(p, feed_id_part, sizeof(feed_id_part) - 1);
            p = ngx_cpymem(p, feed_id, MD5_STR_LEN);
            *p++ = ';';
            ngx_memcpy(p, value->data, value->len);
            value->data = q;
            value->len = len;

            //send feed_id cookie
            len = sizeof(feed_id_part) - 1 + MD5_STR_LEN 
                + sizeof(expires) - 1 
                + sizeof(cookie_path) - 1 ;
            cookie = ngx_pcalloc(r->pool, len);
            if (cookie == NULL) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory!");
                return NGX_ERROR;
            }
            p = ngx_cpymem(cookie, &feed_id_part[1], sizeof(feed_id_part) - 2);
            p = ngx_cpymem(p, feed_id, MD5_STR_LEN);
            p = ngx_cpymem(p, expires, sizeof(expires) - 1);
            ngx_memcpy(p, cookie_path, sizeof(cookie_path) - 1);

            set_cookie = ngx_list_push(&r->headers_out.headers);
            if (set_cookie == NULL) {
                return NGX_ERROR;
            }

            set_cookie->hash = 1;
            ngx_str_set(&set_cookie->key, "Set-Cookie");
            set_cookie->value.len = len;
            set_cookie->value.data = cookie;
        }
    }

    return NGX_OK;
}

FEED_LOG_LINE_VAR_GET(user_agent)
{
    ngx_table_elt_t *user_agent;
    u_char *p;
    
    user_agent = r->headers_in.user_agent;
    if (user_agent->value.len == 0) {
        value->data = (u_char *)"-";
        value->len = 1;
        return NGX_OK;
    }

    value->len = user_agent->key.len + 2 + user_agent->value.len; // ': ' split
    value->data = ngx_pcalloc(r->pool, value->len + 1); //prespace for '\0'

    if (value->data == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory!");
        return NGX_ERROR;
    }

    p = value->data;
    p = ngx_cpymem(p, user_agent->key.data, user_agent->key.len);
    *p++ = ':', *p++ = ' ';
    ngx_memcpy(p, user_agent->value.data, user_agent->value.len);

    return NGX_OK;
}

static ngx_int_t
ngx_http_log_feed_header_filter(ngx_http_request_t *r)
{
    ngx_http_log_feed_srv_conf_t *lfcf;

    ngx_str_t *feed_log_format, *feed_log_root_dir;

    u_char *args;
    ngx_str_t raw_uri, *client_uri;

#if (LOG_HOST_DIR == 1) 
    ngx_str_t *host;
#endif

    ngx_str_t sub_path, sub_dirs, fname, full_dir;

    if (r->headers_out.status >= NGX_HTTP_BAD_REQUEST
            || !(r->method & (NGX_HTTP_GET))
            || r != r->main) {
        goto next_filter;
    }

    lfcf = ngx_http_get_module_srv_conf(r, ngx_http_log_feed_module);

    {
        ngx_uint_t len;
        for (len = 0;
                (len < r->unparsed_uri.len) && (r->unparsed_uri.data[len] != '?'); 
                len++);
        raw_uri.len = len;
        raw_uri.data = ngx_pcalloc(r->pool, raw_uri.len+1); //padding '\0'
        if (raw_uri.data == NULL) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory!");
            return NGX_ERROR;
        }
        ngx_memcpy(raw_uri.data, r->unparsed_uri.data, len);  
    }

    client_uri = &raw_uri;

    //Only serve for /feed.gif$ location
    if (lfcf->log_feed && (client_uri->len >= feed_gif.len)) {
        u_char *client_uri_suffix;
        client_uri_suffix = ngx_pcalloc(r->pool, feed_gif.len+1); //padding '\0'
        if (client_uri_suffix == NULL) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory!");
            return NGX_ERROR;
        }
        ngx_memcpy(client_uri_suffix, 
                client_uri->data + client_uri->len - feed_gif.len, 
                feed_gif.len);
        if (ngx_strncmp(client_uri_suffix, feed_gif.data, feed_gif.len) != 0) {
            goto next_filter;
        }
    } else {
        goto next_filter;
    }

    //prepend indicator '&' to args
    if (r->args.len > 0) {
        args = ngx_pnalloc(r->pool, r->args.len+1);
        if (args == NULL) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory!");
            return NGX_ERROR;
        }
        args[0] = '&';
        ngx_memcpy(&args[1], r->args.data, r->args.len);
        r->args.data = args;
        r->args.len += 1;
    }

    if (client_uri->len == feed_gif.len) {
        sub_dirs.len = 0;
        sub_dirs.data = NULL;
        fname.len = 0;
        fname.data = NULL;
    } else {
        ngx_int_t pos, len;

        len = client_uri->len - feed_gif.len;

        sub_path.len = len;
        sub_path.data = ngx_pcalloc(r->pool, sub_path.len+1); //padding '\0'
        if (sub_path.data == NULL) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory");
            return NGX_ERROR;
        }
        ngx_memcpy(sub_path.data, client_uri->data, sub_path.len);

        for(pos=len-1; pos>=0 && sub_path.data[pos] != '/'; pos--); 

        if (len-1 == pos) {
            sub_dirs.len = sub_path.len;
            sub_dirs.data = sub_path.data;
            fname.len = 0;
            fname.data = NULL; 
        } else {
            sub_dirs.len = pos+1;
            sub_dirs.data = ngx_pcalloc(r->pool, sub_dirs.len+1); //padding '\0'
            if (sub_dirs.data == NULL) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory!");
                return NGX_ERROR;
            }
            ngx_memcpy(sub_dirs.data, sub_path.data, sub_dirs.len);

            fname.len = len-pos-1;
            fname.data = ngx_pcalloc(r->pool, fname.len+1); //padding '\0'
            if (fname.data == NULL) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory");
                return NGX_ERROR;
            }
            ngx_memcpy(fname.data, &sub_path.data[pos+1], fname.len);
        }
    }

    feed_log_root_dir = &lfcf->log_root_dir;

#if (LOG_HOST_DIR == 1)
    host = &r->headers_in.host->value;
    full_dir.len = feed_log_root_dir->len + host->len + sub_dirs.len;
#else
    full_dir.len = feed_log_root_dir->len + sub_dirs.len;
#endif

    full_dir.len += 1; //prespace for '/'

    full_dir.data = ngx_pcalloc(r->pool, full_dir.len+1); //padding '\0'
    if (full_dir.data == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory");
        return NGX_ERROR;
    }
    ngx_memcpy(full_dir.data, feed_log_root_dir->data, feed_log_root_dir->len);
#if (LOG_HOST_DIR == 1)
    ngx_memcpy(&full_dir.data[feed_log_root_dir->len], host->data, host->len);
    ngx_memcpy(&full_dir.data[feed_log_root_dir->len + host->len], 
            sub_dirs.data, sub_dirs.len);
#else
    ngx_memcpy(&full_dir.data[feed_log_root_dir->len], sub_dirs.data, sub_dirs.len);
#endif

    if (full_dir.data[full_dir.len-2] != '/') {
        full_dir.data[full_dir.len-1] = '/'; 
    } else {
        full_dir.len -= 1; //restore the old length
    }

    feed_log_format = &lfcf->log_format;

    if (do_log_feed(&full_dir, &fname, feed_log_format, r) == NGX_ERROR) {
        return NGX_ERROR;
    }

next_filter:
    return ngx_http_next_header_filter(r);
}

static inline ngx_int_t 
feed_log_line_init(feed_log_line_t *line, ngx_http_request_t *r)
{
    line->size = FEED_LOG_LINE_BUF_SIZE;
    line->buf.len = 0;
    line->buf.data = ngx_pcalloc(r->pool, line->size);
    if (line->buf.data == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory!");
        return NGX_ERROR;
    }
    return NGX_OK;
}

#define LINE_BUF_SIZE_CHECK(line, check_len, r) \
    do {                                                            \
        u_char *new_buf;                    \
        ngx_uint_t new_size;                \
        if ((line)->buf.len + check_len > ((line)->size - 2/*prespace for '\n' and '\0'*/)) {\
            new_size = (line)->size * 2;                      \
            new_buf = ngx_pcalloc(r->pool, new_size);       \
            if (new_buf == NULL) {                      \
                ngx_log_error(NGX_LOG_ERR, r->connection->log,  \
                        0, "No more memory!");   \
                return NGX_ERROR;                           \
            } else {                                                    \
                ngx_memcpy(new_buf, (line)->buf.data, (line)->buf.len);         \
                (line)->buf.data = new_buf;                               \
                (line)->size = new_size;                                  \
            }                                           \
        }                   \
    } while(0)

static inline ngx_int_t 
feed_log_line_add_var(feed_log_line_t *line, feed_log_line_var_t **vpp, 
        ngx_str_t *log_format, ngx_int_t *fmt_pos, ngx_http_request_t *r)
{
    feed_log_line_var_t **next_vpp;
    ngx_int_t fmt_prepend_len;

    fmt_prepend_len = (*vpp)->pos - *fmt_pos;
    if (fmt_prepend_len > 0) {
        LINE_BUF_SIZE_CHECK(line, fmt_prepend_len, r);
        ngx_memcpy(line->buf.data + line->buf.len, &log_format->data[*fmt_pos], 
                fmt_prepend_len);
        line->buf.len += fmt_prepend_len;
        *fmt_pos += fmt_prepend_len;
    }

    if ((*vpp)->value.len > 0) {
        LINE_BUF_SIZE_CHECK(line, (*vpp)->value.len, r);
        ngx_memcpy(line->buf.data + line->buf.len, (*vpp)->value.data, (*vpp)->value.len);
        line->buf.len += (*vpp)->value.len;
    }
    *fmt_pos += ((*vpp)->name.len + 1);
    

    next_vpp = vpp + 1;

    if (*next_vpp == NULL) { //last var, append the last fmt str.
        ngx_int_t fmt_append_len = log_format->len - *fmt_pos;
        if (fmt_append_len > 0) {
            LINE_BUF_SIZE_CHECK(line, fmt_append_len, r);
            ngx_memcpy(line->buf.data + line->buf.len, 
                    &log_format->data[*fmt_pos], fmt_append_len);
            line->buf.len += fmt_append_len;
        }
        *fmt_pos = -1; //End of fmt
    }
    return NGX_OK;
}

static ngx_int_t do_log_feed(ngx_str_t *dir, ngx_str_t *fname, ngx_str_t *log_format,
        ngx_http_request_t *r)
{
    feed_log_line_t line;
    ngx_str_t feed_fname;
    ngx_int_t ret;
    
    if (FEED_LOG_PATH_EXISTS(dir->data)) {
        goto dir_exists;
    }

    //create full dir recursively
    {
        ngx_http_log_feed_srv_conf_t *lfcf;
        u_char full_dir_part[MAX_FEED_LOG_FNAME_LEN + 1] = {0, }; 
        size_t offpos;

        lfcf = ngx_http_get_module_srv_conf(r, ngx_http_log_feed_module);
        ngx_memcpy(full_dir_part, lfcf->log_root_dir.data, lfcf->log_root_dir.len);
        
        offpos =  lfcf->log_root_dir.len;

        for (/* void */; offpos <= dir->len; offpos++) {
            full_dir_part[offpos] = dir->data[offpos];
            if (full_dir_part[offpos] != '/' && full_dir_part[offpos] != 0) {
                continue;
            }
            ret = ngx_create_dir(full_dir_part, FEED_LOG_DIR_DEFAULT_ACCESS);
            if (ret == -1) {
                if (ngx_errno != EEXIST) {
                    ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno, 
                            "Create dir %s failed!\n", dir->data);
                    return NGX_ERROR;
                }
            }
        }
    }

dir_exists:
    if (fname->data == NULL) {
        fname->len = sizeof(DEFAULT_FEED_LOG_FNAME) - 1;
        fname->data = (u_char *)DEFAULT_FEED_LOG_FNAME;
    }
    
    feed_fname.len = dir->len + fname->len; //padding '\0'
    feed_fname.data = ngx_pcalloc(r->pool, feed_fname.len+1);
    if (feed_fname.data == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No more memory!");
        return NGX_ERROR;
    }
    ngx_memcpy(feed_fname.data, dir->data, dir->len);
    ngx_memcpy(feed_fname.data + dir->len, fname->data, fname->len);
    
    //generate the feed log line
    {
        feed_log_line_var_t **vpp, *vp;
        ngx_int_t i;
        ngx_int_t fmt_pos = 0;
        ngx_int_t ret;

        ret = feed_log_line_init(&line, r);
        if (ret != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, 
                    "Feed log line init failed!");
            return NGX_ERROR;
        }

        for (i = 0, vp = feed_log_line_vars;
                vp->name.data != NULL;
                vp = &feed_log_line_vars[++i]) {

            if (!vp->valid) {
                continue;
            }
            ret = vp->get_val(&vp->param, &vp->value, r);
            if (ret != NGX_OK) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, 
                        "Get feed log line var %s failed!",
                        vp->name.data);
                return NGX_ERROR;
            }
        }
        
        for (i = 0, vpp = &feed_log_line_vps[0];
                *vpp != NULL;
                vpp = &feed_log_line_vps[++i]) {

            if (!(*vpp)->valid) {
                ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0, 
                        "Invalid feed log line var!");
                return NGX_ERROR;
            } 
            
            ret = feed_log_line_add_var(&line, vpp, log_format, &fmt_pos, r);

            if (ret != NGX_OK) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                        "Feed log line add var %s failed!",
                        (*vpp)->name.data);
                return NGX_ERROR;
            }

        }
        //add the \n char
        line.buf.data[line.buf.len++] = '\n';
    }
   
    //get and write file. 
    {
        feed_log_file_ex_t flf;
        if (feed_log_get_file(&flf, line.buf.len, &feed_fname, &feed_log_shm, r) 
                != NGX_OK) {
            return NGX_ERROR;
        }

        if (feed_log_write_file(&flf, line.buf.data, line.buf.len, r) 
                != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

static ngx_int_t ngx_http_log_feed_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_log_feed_header_filter;

    return NGX_OK;
}

static void *ngx_http_log_feed_create_srv_conf(ngx_conf_t *cf)
{
    ngx_http_log_feed_srv_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_log_feed_srv_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    conf->log_feed = NGX_CONF_UNSET_UINT; 

    conf->log_root.len = 0;
    conf->log_root.data = NULL;

    conf->log_root_dir.len = 0;
    conf->log_root_dir.data = NULL;

    conf->log_format.len = 0;
    conf->log_format.data = NULL;

    return conf;
}

    static char *
ngx_http_log_feed_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_log_feed_srv_conf_t *prev = parent;
    ngx_http_log_feed_srv_conf_t *lfcf = child;

    ngx_conf_merge_uint_value(lfcf->log_feed, prev->log_feed, 0);
    ngx_conf_merge_str_value(lfcf->log_root, prev->log_root, DEFAULT_FEED_LOG_ROOT);
    ngx_conf_merge_str_value(lfcf->log_format, prev->log_format, DEFAULT_FEED_LOG_FORMAT);
    //log_format check and init
    {
        ngx_int_t pos;
        ngx_int_t fmt_len = lfcf->log_format.len;
        ngx_str_t *feed_log_format = &lfcf->log_format;
        ngx_int_t vp_idx = 0;
        for (pos = 0; pos < fmt_len; pos++) {
            if (feed_log_format->data[pos] == '$') {
                ngx_int_t i = ++pos;
                ngx_int_t j = 0;
                ngx_int_t var_len = 0;
                u_char c;
                feed_log_line_var_t *vp;
                for (c=feed_log_format->data[pos]; 
                        ((c >= 'a' && c <= 'z') || c == '_') && pos < fmt_len; 
                        c=feed_log_format->data[++pos]) {
                    var_len++;
                }
                pos--;
                if (!var_len) {
                    return NGX_CONF_ERROR;
                }
                for (vp = &feed_log_line_vars[0];
                        vp->name.data != NULL;
                        vp = &feed_log_line_vars[++j]) {
                        if (ngx_strncmp(vp->name.data, &feed_log_format->data[i], 
                                    var_len) == 0) {

                            vp->valid = 1;
                            vp->pos = i-1; //add the '$'
                            feed_log_line_vps[vp_idx++] = vp;
                            goto found;
                        } else {
                            continue;
                        }
                }
                ngx_log_error(NGX_LOG_ERR, cf->log, 0, "Invalid feed log format: %s",
                        &feed_log_format->data[i]);
                return NGX_CONF_ERROR;
found:
                continue;
            }
        }
    }

    if (lfcf->log_root.data[lfcf->log_root.len-1] != '/')  {
        lfcf->log_root_dir.len = lfcf->log_root.len + 1; //prespace for '/'
        lfcf->log_root_dir.data = ngx_pcalloc(cf->pool, lfcf->log_root_dir.len+1); //padding '\0' 
        if (lfcf->log_root_dir.data == NULL) {
            ngx_log_error(NGX_LOG_ERR, cf->log, 0, "No more memory!");
            return NGX_CONF_ERROR;
        }
        ngx_memcpy(lfcf->log_root_dir.data, lfcf->log_root.data, lfcf->log_root.len);
        lfcf->log_root_dir.data[lfcf->log_root_dir.len-1] = '/';
    } else {
        ngx_conf_merge_str_value(lfcf->log_root_dir, lfcf->log_root, DEFAULT_FEED_LOG_ROOT);
    }

    if (!lfcf->log_feed) { //log_feed off
        return NGX_CONF_OK;
    }

    if (lfcf->log_root_dir.data[0] != '/') { //relative path
        u_char *dir_str;
        size_t len;

        len = cf->cycle->prefix.len + lfcf->log_root_dir.len;
        dir_str = ngx_pcalloc(cf->pool, len+1);  //padding '\0'
        if (dir_str == NULL) {
            ngx_log_error(NGX_LOG_ERR, cf->log, 0, "No more memory!");
            return NGX_CONF_ERROR;
        }

        ngx_memcpy(dir_str, cf->cycle->prefix.data, cf->cycle->prefix.len);
        ngx_memcpy(dir_str+cf->cycle->prefix.len, 
                lfcf->log_root_dir.data, lfcf->log_root_dir.len);

        lfcf->log_root_dir.data = dir_str;
        lfcf->log_root_dir.len = len;
    } 

    {
        //Create the feed log root dir
        int ret;
        ngx_core_conf_t *ccf;
        int old_umask;

        old_umask = umask(0);
        ret = ngx_create_dir(lfcf->log_root_dir.data, FEED_LOG_DIR_DEFAULT_ACCESS);
        umask(old_umask);
        if (ret == -1) {
            if (ngx_errno != EEXIST) {
                ngx_log_error(NGX_LOG_ERR, cf->log, ngx_errno, 
                        "Create dir %s failed!\n", lfcf->log_root_dir.data);
                return NGX_CONF_ERROR;
            }
        }
        //Change the feed log root dir owner to worker process euid and egid
        ccf = (ngx_core_conf_t *) ngx_get_conf(cf->cycle->conf_ctx, ngx_core_module);
        ret = chown((const char *)lfcf->log_root_dir.data, ccf->user, ccf->group); 
        if (ret == -1) {
            ngx_log_error(NGX_LOG_ERR, cf->log, ngx_errno,
                    "Change dir %s uid and gid failed!");
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}
