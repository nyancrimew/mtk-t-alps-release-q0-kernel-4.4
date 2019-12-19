/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

#include "mtk_vcorefs_manager.h"
#include "mtk_spm_vcore_dvfs.h"

/* #define BUILD_ERROR */

static DEFINE_MUTEX(vcorefs_mutex);

#define DEFINE_ATTR_RO(_name)			\
static struct kobj_attribute _name##_attr = {	\
	.attr	= {				\
		.name = #_name,			\
		.mode = 0444,			\
	},					\
	.show	= _name##_show,			\
}

#define DEFINE_ATTR_RW(_name)			\
static struct kobj_attribute _name##_attr = {	\
	.attr	= {				\
		.name = #_name,			\
		.mode = 0644,			\
	},					\
	.show	= _name##_show,			\
	.store	= _name##_store,		\
}

#define __ATTR_OF(_name)	(&_name##_attr.attr)

#ifdef CONFIG_MTK_RAM_CONSOLE
#define VCOREFS_AEE_RR_REC 1
#else
#define VCOREFS_AEE_RR_REC 0
#endif

struct vcorefs_profile {
	int plat_init_opp;
	bool init_done;
	bool init_opp_perf;
	bool autok_lock;
	u32 kr_req_mask;
	u32 kr_log_mask;
	bool fix_hpm_req;
};

static struct vcorefs_profile vcorefs_ctrl = {
	.plat_init_opp	= 0,
	.init_done	= 0,
	.init_opp_perf	= 0,
	.autok_lock	= 0,
	.kr_req_mask	= 0,
	.kr_log_mask	= (1U << LAST_KICKER) | (1U << KIR_GPU),/* mask-off KIR_GPU, LAST_KICKER */
	.fix_hpm_req	= 0,
};

/*
 * __nosavedata will not be restored after IPO-H boot
 */
static bool feature_en __nosavedata;

/*
 * For MET tool register handler
 */
static vcorefs_req_handler_t vcorefs_req_handler;

void vcorefs_register_req_notify(vcorefs_req_handler_t handler)
{
	vcorefs_req_handler = handler;
}
EXPORT_SYMBOL(vcorefs_register_req_notify);

/*
 * Manager extern API
 */
int is_vcorefs_can_work(void)
{
	struct vcorefs_profile *pwrctrl = &vcorefs_ctrl;
	int r = 0;

	mutex_lock(&vcorefs_mutex);

	if (pwrctrl->fix_hpm_req)
		r = -1;		/* as feature disable */
	else if (pwrctrl->init_done && feature_en)
		r = 1;		/* ready to use vcorefs */
	else if (!is_vcorefs_feature_enable())
		r = -1;		/* not support vcorefs */
	else
		r = 0;		/* init not finish, please wait */
	mutex_unlock(&vcorefs_mutex);

	return r;
}

/*
 * AutoK related API
 */
static int vcorefs_autok_lock_dvfs(bool lock)
{
	struct vcorefs_profile *pwrctrl = &vcorefs_ctrl;

	mutex_lock(&vcorefs_mutex);
	pwrctrl->autok_lock = lock;
	mutex_unlock(&vcorefs_mutex);

	return 0;
}

static int vcorefs_autok_set_vcore(int kicker, enum dvfs_opp opp)
{
	struct vcorefs_profile *pwrctrl = &vcorefs_ctrl;
	struct kicker_config krconf;
	int r = 0;

	if (opp >= NUM_OPP || !pwrctrl->autok_lock) {
		vcorefs_crit("[AUTOK] set vcore fail, opp: %d, autok_lock: %d\n", opp, pwrctrl->autok_lock);
		return -1;
	}

	mutex_lock(&vcorefs_mutex);
	krconf.kicker = kicker;
	krconf.opp = opp;
	krconf.dvfs_opp = opp;

	vcorefs_crit("[AUTOK] kicker: %d, opp: %d, dvfs_opp: %d, curr_opp: %d\n",
		     krconf.kicker, krconf.opp, krconf.dvfs_opp, vcorefs_get_sw_opp());

	/*
	 * r = 0: DVFS completed
	 *
	 * LPM to HPM:
	 *      r = -2: step1 DVS fail
	 *      r = -3: step2 DFS fail
	 */
	r = kick_dvfs_by_opp_index(&krconf);
	mutex_unlock(&vcorefs_mutex);

	return r;
}

/*
 * Sub-main function
 */
int _get_dvfs_opp(enum dvfs_kicker kicker, enum dvfs_opp opp)
{
	unsigned int dvfs_opp = UINT_MAX;
	int i, group;
	char table[NUM_KICKER * 4 + 1];
	char *p = table;
	char *buff_end = table + (NUM_KICKER * 4 + 1);

	group = vcorefs_get_dvfs_kicker_group(kicker);

	for (i = 0; i < NUM_KICKER; i++) {
		if (kicker_table[i] < 0 || group != vcorefs_get_dvfs_kicker_group(i))
			continue;

		if (kicker_table[i] < dvfs_opp)
			dvfs_opp = kicker_table[i];
	}

	/* if have no request, set to LPM as default */
	if (dvfs_opp == UINT_MAX)
		dvfs_opp = OPPI_UNREQ;

	for (i = 0; i < NUM_KICKER; i++)
		p += snprintf(p, buff_end-p, "%d, ", kicker_table[i]);

	vcorefs_crit_mask(log_mask(), kicker, "kicker: %d, opp: %d, dvfs_opp: %d, curr_opp: %d, kr opp: %s\n",
						kicker, opp, dvfs_opp, vcorefs_get_sw_opp(), table);
	return dvfs_opp;
}

static int kicker_request_compare(enum dvfs_kicker kicker, enum dvfs_opp opp)
{
	/* compare kicker table opp with request opp (except SYSFS) */
	if (opp == kicker_table[kicker] && kicker != KIR_SYSFSX) {
		vcorefs_crit_mask(log_mask(), LAST_KICKER, "opp no change, kr_tb: %d, kr: %d, opp: %d\n",
			    kicker_table[kicker], kicker, opp);
		return -1;
	}

	kicker_table[kicker] = opp;

	return 0;
}

static int kicker_request_mask(struct vcorefs_profile *pwrctrl, enum dvfs_kicker kicker,
			       enum dvfs_opp opp)
{
	if (pwrctrl->kr_req_mask & (1U << kicker)) {
		if (opp < 0)
			kicker_table[kicker] = opp;

		vcorefs_crit_mask(log_mask(), kicker, "mask request, mask: 0x%x, kr: %d, opp: %d\n",
			    pwrctrl->kr_req_mask, kicker, opp);
		return -1;
	}

	return 0;
}

static void record_kicker_opp_in_aee(int kicker, int opp)
{
#if VCOREFS_AEE_RR_REC
	u32 val;
	/* record opp table */
	if (opp == OPPI_UNREQ)
		opp = 0x3;
	val = aee_rr_curr_vcore_dvfs_opp();
	val &= ~(0x3 << kicker*2);
	val |= opp<<kicker*2;
	aee_rr_rec_vcore_dvfs_opp(val);

	/* record last kicker and opp */
	val = aee_rr_curr_vcore_dvfs_status();
	val &= ~(0xFF00);
	val |= ((((opp << 12) & 0xF000) | (kicker << 8)) & 0xFF00);
	aee_rr_rec_vcore_dvfs_status(val);
#endif
}

/*
 * Main function API (Called by kicker request for DVFS)
 * PASS: return 0
 * FAIL: return less than 0
 */
int vcorefs_request_dvfs_opp(enum dvfs_kicker kicker, enum dvfs_opp opp)
{
	struct vcorefs_profile *pwrctrl = &vcorefs_ctrl;
	struct kicker_config krconf;
	int r;
	int autok_r, autok_lock;

	if (pwrctrl->fix_hpm_req) {
		feature_en = 0;		/* as feature disable */
		vcorefs_crit("Fail @ 0.7V vcore, not allow  kr %d opp: %d\n",  kicker, opp);
		return -1;
	}

	if (is_vcorefs_feature_enable() && !pwrctrl->init_done) {
		vcorefs_crit_mask(log_mask(), kicker, "request before init done(kr:%d opp:%d)\n", kicker, opp);
		if (vcorefs_request_init_opp(kicker, opp))
			return 0;
	}


	if (!feature_en || !pwrctrl->init_done) {
		vcorefs_crit_mask(log_mask(), kicker, "feature_en: %d, init_done: %d, kr: %d, opp: %d\n",
							feature_en, pwrctrl->init_done, kicker, opp);
		return -1;
	}

	autok_r = governor_autok_check(kicker);
	if (autok_r) {
		autok_lock = governor_autok_lock_check(kicker, opp);
		vcorefs_crit("[AUTOK] kr %d set %s\n", kicker, autok_lock ? "lock" : "unlock");

		if (autok_lock) {
			vcorefs_autok_lock_dvfs(autok_lock);
			vcorefs_autok_set_vcore(kicker, opp);
		} else {
			vcorefs_autok_set_vcore(kicker, _get_dvfs_opp(kicker, opp));
			vcorefs_autok_lock_dvfs(autok_lock);
		}
		return 0;
	}

	if (pwrctrl->autok_lock) {
		vcorefs_crit("[AUTOK] autok lock: not allow kr %d, opp: %d\n", kicker, opp);
		return -1;
	}

	if (kicker_request_mask(pwrctrl, kicker, opp))
		return -1;

	if (kicker_request_compare(kicker, opp))
		return 0;

	mutex_lock(&vcorefs_mutex);

	krconf.kicker = kicker;
	krconf.opp = opp;
	krconf.dvfs_opp = _get_dvfs_opp(kicker, opp);

	record_kicker_opp_in_aee(kicker, opp);
	if (vcorefs_req_handler)
		vcorefs_req_handler(kicker, opp);

	/*
	 * r = 0: DVFS completed
	 *
	 * LPM to HPM:
	 *      r = -2: step1 DVS fail
	 *      r = -3: step2 DFS fail
	 */
	r = kick_dvfs_by_opp_index(&krconf);

	mutex_unlock(&vcorefs_mutex);

	return r;
}

/*
 * Called by governor for init flow
 */
void vcorefs_drv_init(bool plat_init_done, bool plat_feature_en, int plat_init_opp)
{
	struct vcorefs_profile *pwrctrl = &vcorefs_ctrl;
	int init_opp = vcorefs_bootup_get_init_opp();

	mutex_lock(&vcorefs_mutex);
	if (!plat_feature_en)
		feature_en = 0;
	else
		feature_en = 1;

#if VCOREFS_AEE_RR_REC
	aee_rr_rec_vcore_dvfs_opp(0xffffffff);
#endif

	if (init_opp != OPPI_UNREQ) {
		/* GPU kicker already request HPM in init */
		kicker_table[KIR_BOOTUP] = init_opp;
		record_kicker_opp_in_aee(KIR_BOOTUP, init_opp);
	}

	pwrctrl->plat_init_opp	= plat_init_opp;
	pwrctrl->init_done	= plat_init_done;
	mutex_unlock(&vcorefs_mutex);

	vcorefs_crit("[%s] init_done: %d, feature_en: %d\n", __func__, pwrctrl->init_done, feature_en);

	if (feature_en)
		governor_autok_manager();

	vcorefs_bootup_set_init_opp(OPPI_UNREQ);
}

/*
 * Vcorefs debug sysfs
 */
static char *vcorefs_get_kicker_info(char *p)
{
	int i;
	char *buff_end = p + PAGE_SIZE;

	for (i = 0; i < NUM_KICKER; i++)
		p += snprintf(p, buff_end - p,
			"[%.32s] opp: %d\n", governor_get_kicker_name(i), kicker_table[i]);

	return p;
}

u32 log_mask(void)
{
	struct vcorefs_profile *pwrctrl = &vcorefs_ctrl;

	return pwrctrl->kr_log_mask;
}

static ssize_t vcore_debug_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct vcorefs_profile *pwrctrl = &vcorefs_ctrl;
	char *p = buf;
	char *buff_end = buf + PAGE_SIZE;

	p += snprintf(p, buff_end - p, "\n");
	p += snprintf(p, buff_end - p,
		"[feature_en   ]: %d\n", feature_en);
	p += snprintf(p, buff_end - p,
		"[plat_init_opp]: %d\n", pwrctrl->plat_init_opp);
	p += snprintf(p, buff_end - p,
		"[init_done    ]: %d\n", pwrctrl->init_done);
	p += snprintf(p, buff_end - p,
		"[init_opp_perf]: %d\n", pwrctrl->init_opp_perf);
	p += snprintf(p, buff_end - p,
		"[autok_lock   ]: %d\n", pwrctrl->autok_lock);
	p += snprintf(p, buff_end - p,
		"[kr_req_mask  ]: 0x%x\n", pwrctrl->kr_req_mask);
	p += snprintf(p, buff_end - p,
		"[kr_log_mask  ]: 0x%x\n", pwrctrl->kr_log_mask);
	p += snprintf(p, buff_end - p,
		"[fix_hpm_req  ]: %d\n", pwrctrl->fix_hpm_req);
	p += snprintf(p, buff_end - p, "\n");

	p = governor_get_dvfs_info(p);
	p += snprintf(p, buff_end - p, "\n");

	p = vcorefs_get_kicker_info(p);
	p += snprintf(p, buff_end - p, "\n");

#if VCOREFS_AEE_RR_REC
	p += snprintf(p, buff_end - p,
		"[aee]vcore_dvfs_opp   : 0x%x\n", aee_rr_curr_vcore_dvfs_opp());
	p += snprintf(p, buff_end - p,
		"[aee]vcore_dvfs_status: 0x%x\n", aee_rr_curr_vcore_dvfs_status());
	p += snprintf(p, buff_end - p,
		"\n");
#endif

	p = spm_vcorefs_dump_dvfs_regs(p);

	return p - buf;
}

static ssize_t vcore_debug_store(struct kobject *kobj, struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	struct vcorefs_profile *pwrctrl = &vcorefs_ctrl;
	struct kicker_config krconf;
	int kicker, val, r, opp;
	char cmd[32];
	int change = 0;

	/* governor debug sysfs */
	r = governor_debug_store(buf);
	if (!r)
		return count;

	if (!(sscanf(buf, "%31s 0x%x", cmd, &val) == 2 ||
		   sscanf(buf, "%31s %d", cmd, &val) == 2)) {
		return -EPERM;
	}

	/* manager debug sysfs */
	vcorefs_crit("vcore_debug: cmd: %s, val: %d\n", cmd, val);

	if (!strcmp(cmd, "feature_en")) {
		mutex_lock(&vcorefs_mutex);
		if (val && is_vcorefs_feature_enable() && (!feature_en)) {
			kicker_table[KIR_SYSFS] = OPP_OFF;
			opp = _get_dvfs_opp(KIR_SYSFS, OPP_OFF);
			change = 1;
		} else if ((!val) && feature_en) {
			kicker_table[KIR_SYSFS] = OPP_0;
			opp = OPP_0;
			change = 1;
		} else {
			change = 0;
		}

		if (change == 1) {
			krconf.kicker = KIR_SYSFS;
			krconf.opp = kicker_table[KIR_SYSFS];
			krconf.dvfs_opp = opp;

			vcorefs_crit("kicker: %d, opp: %d, dvfs_opp: %d, curr_opp: %d\n",
					krconf.kicker, krconf.opp, krconf.dvfs_opp, vcorefs_get_sw_opp());

			/*
			 * r = 0: DVFS completed
			 *
			 * LPM to HPM:
			 *      r = -2: step1 DVS fail
			 *      r = -3: step2 DFS fail
			 */
			r = kick_dvfs_by_opp_index(&krconf);
			feature_en = !!(val);
		}
		mutex_unlock(&vcorefs_mutex);
	} else if (!strcmp(cmd, "kr_req_mask")) {
		mutex_lock(&vcorefs_mutex);
		pwrctrl->kr_req_mask = val;
		mutex_unlock(&vcorefs_mutex);
	} else if (!strcmp(cmd, "kr_log_mask")) {
		mutex_lock(&vcorefs_mutex);
		pwrctrl->kr_log_mask = val;
		mutex_unlock(&vcorefs_mutex);
	} else if (!strcmp(cmd, "KIR_SYSFSX") && (val >= OPP_OFF && val < NUM_OPP)) {
		mutex_lock(&vcorefs_mutex);
		if (val != OPP_OFF)
			pwrctrl->kr_req_mask = (1U << NUM_KICKER) - 1;
		else
			pwrctrl->kr_req_mask = 0;

		krconf.kicker = KIR_SYSFSX;
		krconf.opp = val;
		krconf.dvfs_opp = val;

		kicker_table[KIR_SYSFSX] = val;

		vcorefs_crit("kicker: %d, opp: %d, dvfs_opp: %d, curr_opp: %d\n",
				krconf.kicker, krconf.opp, krconf.dvfs_opp, vcorefs_get_sw_opp());

		/*
		 * r = 0: DVFS completed
		 *
		 * LPM to HPM:
		 *      r = -2: step1 DVS fail
		 *      r = -3: step2 DFS fail
		 */
		r = kick_dvfs_by_opp_index(&krconf);

		mutex_unlock(&vcorefs_mutex);
	} else {
		/* set kicker opp and do DVFS */
		kicker = vcorefs_output_kicker_id(cmd);
		if (kicker != -1)
			vcorefs_request_dvfs_opp(kicker, val);
	}

	return count;
}

static ssize_t opp_table_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	char *p = buf;

	p = vcorefs_get_opp_table_info(p);

	return p - buf;
}

static ssize_t opp_table_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	int val;
	char cmd[32];

	if (sscanf(buf, "%31s %u", cmd, &val) != 2)
		return -EPERM;

	vcorefs_crit("opp_table: cmd: %s, val: %d (0x%x)\n", cmd, val, val);

	mutex_lock(&vcorefs_mutex);
	if (!feature_en)
		vcorefs_update_opp_table(cmd, val);
	mutex_unlock(&vcorefs_mutex);

	return count;
}

static ssize_t opp_num_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	char *p = buf;
	int num = vcorefs_get_num_opp();
	char *buff_end = p + PAGE_SIZE;

	p += snprintf(p, buff_end - p, "%d\n", num);

	return p - buf;
}

DEFINE_ATTR_RW(vcore_debug);
DEFINE_ATTR_RW(opp_table);
DEFINE_ATTR_RO(opp_num);

static struct attribute *vcorefs_attrs[] = {
	__ATTR_OF(opp_table),
	__ATTR_OF(vcore_debug),
	__ATTR_OF(opp_num),
	NULL,
};

static struct attribute_group vcorefs_attr_group = {
	.name = "vcorefs",
	.attrs = vcorefs_attrs,
};

int init_vcorefs_sysfs(void)
{
	int r;

	r = sysfs_create_group(power_kobj, &vcorefs_attr_group);

	return 0;
}

int vcorefs_fix_hpm_req(void)
{
	struct vcorefs_profile *pwrctrl = &vcorefs_ctrl;

	vcorefs_crit("vcorefs_fix_hpm_req\n");
	pwrctrl->fix_hpm_req = 1;
	pwrctrl->kr_req_mask = (1U << NUM_KICKER) - 1;
	spm_vcorefs_set_opp_fix(OPPI_PERF, VCORE_0_P_80_UV, FDDR_S0_KHZ);
	return 0;
}

int vcorefs_get_fix_hpm_req(void)
{
	struct vcorefs_profile *pwrctrl = &vcorefs_ctrl;

	return pwrctrl->fix_hpm_req;
}
