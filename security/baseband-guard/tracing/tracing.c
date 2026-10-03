// SPDX-License-Identifier: GPL-2.0
/*
 * Based on vc-teahouse/Baseband-guard a54e0dc6cf0aff4dd87fec49644a02d2eb612905.
 * EVONIX: standard LSM credential blobs and policy-generation-aware SID lookup.
 */
#include "kernel_compat.h"
#include "tracing.h"
#include <linux/mutex.h>

struct lsm_blob_sizes bbg_blob_sizes __ro_after_init = {
	.lbs_cred = sizeof(struct bbg_cred_security_struct),
};

static DEFINE_MUTEX(bbg_sid_lock);
static u32 bbg_policy_seq;
static bool bbg_sids_valid;
static u32 bbg_root_sids[3];
static const char * const bbg_root_contexts[] = {
	"u:r:su:s0", "u:r:magisk:s0", "u:r:ksu:s0",
};

int bb_cred_prepare(struct cred *new, const struct cred *old, gfp_t gfp)
{
	*bbg_cred(new) = *bbg_cred(old);
	return 0;
}

void bb_cred_transfer(struct cred *new, const struct cred *old)
{
	*bbg_cred(new) = *bbg_cred(old);
}

int bb_bprm_set_creds(struct linux_binprm *bprm)
{
	const struct task_security_struct *old = selinux_cred(current_cred());
	const struct task_security_struct *new = selinux_cred(bprm->cred);
	struct bbg_cred_security_struct *next = bbg_cred(bprm->cred);
	u32 seq, end_seq, resolved[ARRAY_SIZE(bbg_root_sids)];
	unsigned int i;
	bool untrusted = false;

	next->is_untrusted_process = bbg_cred(current_cred())->is_untrusted_process;
	if (next->is_untrusted_process || !selinux_initialized())
		return 0;

	/*
	 * Lookup happens only on exec, never on ordinary read/write paths.
	 * Refresh missing contexts when KernelSU/Magisk updates the policy;
	 * do not permanently cache an early-boot context lookup failure.
	 */
	mutex_lock(&bbg_sid_lock);
	seq = avc_policy_seqno();
	if (!bbg_sids_valid || bbg_policy_seq != seq) {
		do {
			seq = avc_policy_seqno();
			for (i = 0; i < ARRAY_SIZE(bbg_root_sids); i++) {
				resolved[i] = 0;
				if (security_secctx_to_secid(bbg_root_contexts[i],
						strlen(bbg_root_contexts[i]), &resolved[i]))
					resolved[i] = 0;
			}
			end_seq = avc_policy_seqno();
		} while (seq != end_seq);
		memcpy(bbg_root_sids, resolved, sizeof(resolved));
		bbg_policy_seq = seq;
		bbg_sids_valid = true;
	}
	for (i = 0; i < ARRAY_SIZE(bbg_root_sids); i++) {
		u32 sid = bbg_root_sids[i];

		if (sid && (old->sid == sid || old->osid == sid ||
			    new->sid == sid || new->osid == sid)) {
			untrusted = true;
			break;
		}
	}
	mutex_unlock(&bbg_sid_lock);
	if (untrusted)
		next->is_untrusted_process = 1;
	return 0;
}
