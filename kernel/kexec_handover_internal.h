/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_KEXEC_HANDOVER_INTERNAL_H
#define LINUX_KEXEC_HANDOVER_INTERNAL_H

#include <linux/kexec_handover.h>
#include <linux/list.h>
#include <linux/types.h>

#ifdef CONFIG_KEXEC_HANDOVER_DEBUGFS
#include <linux/debugfs.h>

struct kho_debugfs {
	struct dentry *dir;
	struct dentry *sub_fdt_dir;
	struct list_head fdt_list;
};

#else
struct kho_debugfs {};
#endif

extern struct kho_scratch *kho_scratch;
extern unsigned int kho_scratch_cnt;

bool kho_finalized(void);

#ifdef CONFIG_KEXEC_HANDOVER_DEBUGFS
int kho_debugfs_init(void);
void kho_in_debugfs_init(struct kho_debugfs *dbg, const void *fdt);
int kho_out_debugfs_init(struct kho_debugfs *dbg);
int kho_debugfs_fdt_add(struct kho_debugfs *dbg, const char *name,
			const void *fdt, bool root);
void kho_debugfs_cleanup(struct kho_debugfs *dbg);
#else
static inline int kho_debugfs_init(void) { return 0; }
static inline void kho_in_debugfs_init(struct kho_debugfs *dbg,
				       const void *fdt) { }
static inline int kho_out_debugfs_init(struct kho_debugfs *dbg) { return 0; }
static inline int kho_debugfs_fdt_add(struct kho_debugfs *dbg, const char *name,
				      const void *fdt, bool root) { return 0; }
static inline void kho_debugfs_cleanup(struct kho_debugfs *dbg) {}
#endif /* CONFIG_KEXEC_HANDOVER_DEBUGFS */

#endif /* LINUX_KEXEC_HANDOVER_INTERNAL_H */
