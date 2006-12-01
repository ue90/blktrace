/*
 * blktrace output analysis: generate a timeline & gather statistics
 *
 * Copyright (C) 2006 Alan D. Brunelle <Alan.Brunelle@hp.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "blktrace.h"
#include "rbtree.h"
#include "list.h"

#define BIT_TIME(t)	((double)SECONDS(t) + ((double)NANO_SECONDS(t) / 1.0e9))

#define BIT_START(iop)	((iop)->t.sector)
#define BIT_END(iop)	((iop)->t.sector + ((iop)->t.bytes >> 9))
#define IOP_READ(iop)	((iop)->t.action & BLK_TC_ACT(BLK_TC_READ))
#define IOP_RW(iop)	(IOP_READ(iop) ? 1 : 0)

#define TO_SEC(nanosec)	((double)(nanosec) / 1.0e9)
#define TO_MSEC(nanosec) (1000.0 * TO_SEC(nanosec))

#if defined(DEBUG)
#define DBG_PING()	dbg_ping()
#define ASSERT(truth)   do {						\
				if (!(truth)) {				\
					DBG_PING();			\
					assert(truth);			\
				}					\
			} while (0)


#define LIST_DEL(hp)	list_del(hp)
#else
#define ASSERT(truth)
#define DBG_PING()
#define LIST_DEL(hp)	do {						\
				if (((hp)->next != NULL) &&		\
				    ((hp)->next != LIST_POISON1))	\
					list_del(hp);			\
			} while (0)
#endif

enum iop_type {
	IOP_Q = 0,
	IOP_X = 1,
	IOP_A = 2,
	IOP_L = 3,	// Betwen-device linkage
	IOP_M = 4,
	IOP_I = 5,
	IOP_D = 6,
	IOP_C = 7,
	IOP_R = 8,
};
#define N_IOP_TYPES	(IOP_R + 1)

struct file_info {
	struct file_info *next;
	FILE *ofp;
	char oname[1];
};

struct mode {
	int most_seeks, nmds;
	long long *modes;
};

struct io;
struct io_list {
	struct list_head head;
	struct io *iop;
	int cy_users;
};

struct avg_info {
	__u64 min, max, total;
	double avg;
	int n;
};

struct avgs_info {
        struct avg_info q2q;
	struct avg_info q2c;
	struct avg_info q2a;		/* Q to (A or X) */
	struct avg_info q2i;		/* Q to (I or M) */
	struct avg_info i2d;		/* (I or M) to D */
	struct avg_info d2c;

	struct avg_info blks;		/* Blocks transferred */
};

struct range_info {
	struct list_head head;		/* on: qranges OR cranges */
	__u64 start, end;
};

struct region_info {
	struct list_head qranges;
	struct list_head cranges;
	struct range_info *qr_cur, *cr_cur;
};

struct p_info {
	struct region_info regions;
	struct avgs_info avgs;
	__u64 last_q;
	__u32 pid;
	char name[1];
};

struct devmap {
	struct devmap *next;
	unsigned int host, bus, target, lun, irq, cpu;
	char model[64];
	char device[32], node[32], pci[32], devno[32];
};

struct stats {
	__u64 rqm[2], ios[2], sec[2], wait, svctm;
	double last_qu_change, last_dev_change, tot_qusz, idle_time;
	int cur_qusz, cur_dev;
};

struct stats_t {
	double n;
	double rqm_s[2], ios_s[2], sec_s[2];
	double avgrq_sz, avgqu_sz, await, svctm, p_util;
};

struct d_info {
	struct list_head all_head, hash_head;
	void *heads;
	struct region_info regions;
	struct devmap *map;
	void *seek_handle;
	FILE *d2c_ofp, *q2c_ofp;
	struct avgs_info avgs;
	struct stats stats, all_stats;
	__u64 last_q, n_ds;
	__u32 device;
};

struct io {
	struct rb_node rb_node;
	struct list_head f_head;
	struct d_info *dip;
	struct p_info *pip;
	struct blk_io_trace t;
	void *pdu;
	enum iop_type type;

	struct list_head down_head, up_head, c_pending, retry;
	struct list_head down_list, up_list;
	__u64 bytes_left;
	int run_ready, linked, self_remap, displayed;
};

/* bt_timeline.c */

extern char bt_timeline_version[], *devices, *exes, *input_name, *output_name;
extern char *seek_name, *iostat_name, *d2c_name, *q2c_name, *per_io_name;
extern double range_delta;
extern FILE *ranges_ofp, *avgs_ofp, *iostat_ofp, *per_io_ofp;;
extern int verbose, ifd, dump_level;
extern unsigned int n_devs;
extern unsigned long n_traces;
extern struct list_head all_devs, all_procs, retries;
extern struct avgs_info all_avgs;
extern __u64 last_q;
extern struct region_info all_regions;
extern struct list_head free_ios;
extern __u64 iostat_interval, iostat_last_stamp;
extern time_t genesis, last_vtrace;

/* args.c */
void handle_args(int argc, char *argv[]);

/* dev_map.c */
int dev_map_read(char *fname);
struct devmap *dev_map_find(__u32 device);

/* devs.c */
void init_dev_heads(void);
struct d_info *dip_add(__u32 device, struct io *iop);
void dip_rem(struct io *iop);
struct d_info *__dip_find(__u32 device);
void dip_foreach_list(struct io *iop, enum iop_type type, struct list_head *hd);
void dip_foreach(struct io *iop, enum iop_type type, 
		 void (*fnc)(struct io *iop, struct io *this), int rm_after);
struct io *dip_find_sec(struct d_info *dip, enum iop_type type, __u64 sec);
void dip_foreach_out(void (*func)(struct d_info *, void *), void *arg);

/* dip_rb.c */
int rb_insert(struct rb_root *root, struct io *iop);
struct io *rb_find_sec(struct rb_root *root, __u64 sec);
void rb_foreach(struct rb_node *n, struct io *iop, 
		      void (*fnc)(struct io *iop, struct io *this),
		      struct list_head *head);

/* iostat.c */
void iostat_init(void);
void iostat_insert(struct io *iop);
void iostat_merge(struct io *iop);
void iostat_issue(struct io *iop);
void iostat_unissue(struct io *iop);
void iostat_complete(struct io *d_iop, struct io *c_iop);
void iostat_check_time(__u64 stamp);
void iostat_dump_stats(__u64 stamp, int all);

/* latency.c */
void latency_init(struct d_info *dip);
void latency_clean(void);
void latency_d2c(struct d_info *dip, __u64 tstamp, __u64 latency);
void latency_q2c(struct d_info *dip, __u64 tstamp, __u64 latency);

/* misc.c */
struct blk_io_trace *convert_to_cpu(struct blk_io_trace *t);
int in_devices(struct blk_io_trace *t);
unsigned int do_read(int ifd, void *buf, int len);
void add_file(struct file_info **fipp, FILE *fp, char *oname);
void clean_files(struct file_info **fipp);
void dbg_ping(void);

/* output.c */
int output_avgs(FILE *ofp);
int output_ranges(FILE *ofp);
char *make_dev_hdr(char *pad, size_t len, struct d_info *dip);

/* proc.c */
void add_process(__u32 pid, char *name);
struct p_info *find_process(__u32 pid, char *name);
void pip_update_q(struct io *iop);
void pip_foreach_out(void (*f)(struct p_info *, void *), void *arg);

/* seek.c */
void *seeki_init(__u32 device);
void seek_clean(void);
void seeki_add(void *handle, struct io *iop);
double seeki_mean(void *handle);
long long seeki_nseeks(void *handle);
long long seeki_median(void *handle);
int seeki_mode(void *handle, struct mode *mp);

/* trace.c */
void dump_iop(FILE *ofp, struct io *to_iop, struct io *from_iop, int indent);
void release_iops(struct list_head *del_head);
void add_trace(struct io *iop);

/* trace_complete.c */
void trace_complete(struct io *c_iop);
int retry_complete(struct io *c_iop);
int ready_complete(struct io *c_iop, struct io *top);
void run_complete(struct io *c_iop);

/* trace_im.c */
void trace_insert(struct io *i_iop);
void trace_merge(struct io *m_iop);
int ready_im(struct io *im_iop, struct io *top);
void run_im(struct io *im_iop, struct io *top, struct list_head *del_head);
void run_unim(struct io *im_iop, struct list_head *del_head);

/* trace_issue.c */
void trace_issue(struct io *d_iop);
int ready_issue(struct io *d_iop, struct io *top);
void run_issue(struct io *d_iop, struct io *top, struct list_head *del_head);
void run_unissue(struct io *d_iop, struct list_head *del_head);

/* trace_queue.c */
void trace_queue(struct io *q_iop);
int ready_queue(struct io *q_iop, struct io *top);
void run_queue(struct io *q_iop, struct io *top, struct list_head *del_head);
void run_unqueue(struct io *q_iop, struct list_head *del_head);

/* trace_remap.c */
void trace_remap(struct io *a_iop);
int ready_remap(struct io *a_iop, struct io *top);
void run_remap(struct io *a_iop, struct io *top, struct list_head *del_head);
void run_unremap(struct io *a_iop, struct list_head *del_head);

/* trace_requeue.c */
void trace_requeue(struct io *r_iop);
int retry_requeue(struct io *r_iop);
int ready_requeue(struct io *r_iop, struct io *top);
void run_requeue(struct io *r_iop);

#include "inlines.h"
