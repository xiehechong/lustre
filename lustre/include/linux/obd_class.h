#ifndef __LINUX_CLASS_OBD_H
#define __LINUX_CLASS_OBD_H

#include <linux/fs.h>
#include <linux/ext2_fs.h>

#include <linux/obd.h>

#include <linux/obd_sim.h>
#include <linux/obd_snap.h>
/* #include <linux/obd_fc.h> */
#include <linux/obd_raid1.h>


#define OBD_PSDEV_MAJOR 120
#define MAX_OBD_DEVICES 2
#define MAX_MULTI 16

typedef unsigned long objid;
typedef struct inode obdattr;

extern struct obd_device obd_dev[MAX_OBD_DEVICES];



#define OBD_ATTACHED 0x1
#define OBD_SET_UP   0x2

/* corresponds to one of the obdx */
struct obd_device {
	struct obd_type *obd_type;
	int obd_minor;
	int obd_flags;
	int obd_refcnt; 
	int obd_multi_count;
	struct obd_device *obd_multi_dev[MAX_MULTI];
	struct obd_conn_info obd_multi_conns[MAX_MULTI];
	unsigned int obd_gen_last_id;
	unsigned long obd_gen_prealloc_quota;
	struct list_head obd_gen_clients;
	union {
		struct sim_obd sim;
		struct raid1_obd raid1;
		struct snap_obd snap;
		/* struct fc_obd fc; */
	} u;
};

struct obd_ops {
	int (*o_attach)(struct obd_device *, int len, void *);
	int (*o_format)(struct obd_device *, int len, void *);
	int (*o_partition)(struct obd_device *, int len, void *);
	int (*o_connect)(struct obd_device *, struct obd_conn_info *info);
	int (*o_disconnect)(unsigned int conn_id);
	int (*o_setup) (struct obd_device *dev, int len, void *data);
	int (*o_cleanup)(struct obd_device *dev);
	int (*o_setattr)(unsigned int conn_id, unsigned long id, struct inode *iattr);
	int (*o_getattr)(unsigned int conn_id, unsigned long id, struct inode *iattr);
	int (*o_statfs)(unsigned int conn_id, struct statfs *statfs);
	int (*o_create)(int conn_id, int prealloc_ino, int *er);
	int (*o_destroy)(unsigned int conn_id, unsigned long ino);
	unsigned long (*o_read)(unsigned int conn_id, unsigned long ino, char *buf, unsigned long count, loff_t offset, int *err);
	unsigned long (*o_read2)(unsigned int conn_id, unsigned long ino, char *buf, unsigned long count, loff_t offset, int *err);
	unsigned long (*o_write)(unsigned int conn_id, unsigned long ino, char *buf, unsigned long count, loff_t offset, int *err);
	int (*o_brw)(int rw, int conn, int objectid, struct page *page, int create);
	long (*o_preallocate)(unsigned int conn_id, int req, long inodes[32], int *err);
	int (*o_cleanup_device)(struct obd_device *);
	int  (*o_get_info)(unsigned int conn_id, int keylen, void *key, int *vallen, void **val);
	int  (*o_set_info)(unsigned int conn_id, int keylen, void *key, int vallen, void *val);
};

#define OBP(dev,op) dev->obd_type->typ_ops->o_ ## op

int obd_register_type(struct obd_ops *ops, char *nm);
int obd_unregister_type(char *nm);

struct obd_client {
	struct list_head cli_chain;
	struct obd_device *cli_obd;
	unsigned int cli_id;
	unsigned long cli_prealloc_quota;
	struct list_head cli_prealloc_inodes;
};

struct obd_prealloc_inode {
	struct list_head obd_prealloc_chain;
	unsigned long inode;
};

/* generic operations shared by various OBD types */
int gen_connect (struct obd_device *obddev, 
		     struct obd_conn_info * conninfo);
int gen_disconnect(unsigned int conn_id);
int gen_multi_setup(struct obd_device *obddev, int len, void *data);
int gen_multi_cleanup(struct obd_device *obddev);
int gen_multi_attach(struct obd_device *obddev, int len, void *data);
struct obd_client *gen_client(int cli_id);
int gen_multi_cleanup_device(struct obd_device *obddev);
int gen_cleanup(struct obd_device *obddev);



/*
 * ioctl commands
 */
struct oic_attach {
	int  att_typelen;
	void *att_type;
	int  att_datalen;
	void *att_data;
};

struct oic_prealloc_s {
	unsigned long cli_id;
	unsigned long alloc; /* user sets it to the number of inodes requesting
		     * to be preallocated.  kernel sets it to the actual number
		     * of succesfully preallocated inodes */
	long inodes[32]; /* actual inode numbers */
};

struct oic_create_s {
	unsigned int conn_id;
	unsigned long prealloc;
};

struct oic_attr_s {
	unsigned int conn_id;
	unsigned long ino;
	struct iattr iattr;
};
struct oic_rw_s {
	unsigned int conn_id;
	unsigned long inode;
	char * buf;
	unsigned long count;
	loff_t offset;
};
struct oic_partition {
	int partition;
	unsigned int size;
};


#define OBD_IOC_CREATE                 _IOR ('f',  3, long)
#define OBD_IOC_SETUP_OBDDEV           _IOW ('f',  4, long)
#define OBD_IOC_CLEANUP                _IO  ('f',  5      )
#define OBD_IOC_DESTROY                _IOW ('f',  6, long)
#define OBD_IOC_PREALLOCATE            _IOWR('f',  7, long)
#define OBD_IOC_DEC_USE_COUNT          _IO  ('f',  8      )
#define OBD_IOC_SETATTR                _IOW ('f',  9, long)
#define OBD_IOC_GETATTR                _IOR ('f', 10, long)
#define OBD_IOC_READ                   _IOWR('f', 11, long)
#define OBD_IOC_WRITE                  _IOWR('f', 12, long)
#define OBD_IOC_CONNECT                _IOR ('f', 13, long)
#define OBD_IOC_DISCONNECT             _IOW ('f', 14, long)
#define OBD_IOC_STATFS                 _IOWR('f', 15, long)
#define OBD_IOC_SYNC                   _IOR ('f',  16, long)
#define OBD_IOC_READ2                  _IOWR('f', 17, long)
#define OBD_IOC_FORMAT                 _IOWR('f', 18, long)
#define OBD_IOC_PARTITION              _IOWR('f', 19, long)
#define OBD_IOC_ATTACH                 _IOWR('f', 20, long)

#define OBD_IOC_DEC_FS_USE_COUNT       _IO  ('f', 32      )


/* sysctl.c */
extern void obd_sysctl_init (void);
extern void obd_sysctl_clean (void);


#endif /* __LINUX_CLASS_OBD_H */
