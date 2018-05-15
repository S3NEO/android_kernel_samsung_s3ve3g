/* Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
 * Copyright (c) 2015 Francisco Franco
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/qpnp/qpnp-adc.h>
#include <linux/msm_thermal.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/android_alarm.h>
#include <linux/thermal.h>
#include <mach/rpm-regulator.h>
#include <mach/rpm-regulator-smd.h>
#include <linux/regulator/consumer.h>
#include <linux/msm_thermal_ioctl.h>

#define MAX_CURRENT_UA 1000000
#define MAX_RAILS 5
#define MAX_THRESHOLD 2

static struct msm_thermal_data msm_thermal_info;
static struct delayed_work check_temp_work;
static struct delayed_work temp_log_work;
static bool core_control_enabled;
static uint32_t cpus_offlined;
static DEFINE_MUTEX(core_control_mutex);
static uint32_t wakeup_ms;
static struct alarm thermal_rtc;
static struct kobject *tt_kobj;
static struct kobject *cc_kobj;
static struct work_struct timer_work;
static struct task_struct *hotplug_task;
static struct task_struct *freq_mitigation_task;
static struct completion hotplug_notify_complete;
static struct completion freq_mitigation_complete;

static int enabled;
static int rails_cnt;
static int psm_rails_cnt;
static int ocr_rail_cnt;
static int limit_idx;
static int limit_idx_low;
static int limit_idx_high;
static int max_tsens_num;
static struct cpufreq_frequency_table *table;
static uint32_t usefreq;
static int freq_table_get;
static bool vdd_rstr_enabled;
static bool vdd_rstr_nodes_called;
static bool vdd_rstr_probed;
static bool psm_enabled;
static bool psm_nodes_called;
static bool psm_probed;
static bool hotplug_enabled;
static bool freq_mitigation_enabled;
static bool ocr_enabled;
static bool ocr_nodes_called;
static bool ocr_probed;
static int *tsens_id_map;
static DEFINE_MUTEX(vdd_rstr_mutex);
static DEFINE_MUTEX(psm_mutex);
static DEFINE_MUTEX(ocr_mutex);
static uint32_t min_freq_limit;

enum thermal_threshold {
	HOTPLUG_THRESHOLD_HIGH,
	HOTPLUG_THRESHOLD_LOW,
	FREQ_THRESHOLD_HIGH,
	FREQ_THRESHOLD_LOW,
	THRESHOLD_MAX_NR,
};

struct cpu_info {
	uint32_t cpu;
	const char *sensor_type;
	uint32_t sensor_id;
	bool offline;
	bool user_offline;
	bool hotplug_thresh_clear;
	struct sensor_threshold threshold[THRESHOLD_MAX_NR];
	bool max_freq;
	uint32_t user_max_freq;
	uint32_t user_min_freq;
	uint32_t limited_max_freq;
	uint32_t limited_min_freq;
	bool freq_thresh_clear;
};

struct rail {
	const char *name;
	uint32_t freq_req;
	uint32_t min_level;
	uint32_t num_levels;
	int32_t curr_level;
	uint32_t levels[3];
	struct kobj_attribute value_attr;
	struct kobj_attribute level_attr;
	struct regulator *reg;
	struct attribute_group attr_gp;
};

struct psm_rail {
	const char *name;
	uint8_t init;
	uint8_t mode;
	struct kobj_attribute mode_attr;
	struct rpm_regulator *reg;
	struct regulator *phase_reg;
	struct attribute_group attr_gp;
};

static struct psm_rail *psm_rails;
static struct psm_rail *ocr_rails;
static struct rail *rails;
static struct cpu_info cpus[NR_CPUS];

struct vdd_rstr_enable {
	struct kobj_attribute ko_attr;
	uint32_t enabled;
};

/* For SMPS only*/
enum PMIC_SW_MODE {
	PMIC_AUTO_MODE  = RPM_REGULATOR_MODE_AUTO,
	PMIC_IPEAK_MODE = RPM_REGULATOR_MODE_IPEAK,
	PMIC_PWM_MODE   = RPM_REGULATOR_MODE_HPM,
};

enum ocr_request {
	OPTIMUM_CURRENT_MIN,
	OPTIMUM_CURRENT_MAX,
	OPTIMUM_CURRENT_NR,
};

#define VDD_RES_RO_ATTRIB(_rail, ko_attr, j, _name) \
	ko_attr.attr.name = __stringify(_name); \
	ko_attr.attr.mode = 444; \
	ko_attr.show = vdd_rstr_reg_##_name##_show; \
	ko_attr.store = NULL; \
	sysfs_attr_init(&ko_attr.attr); \
	_rail.attr_gp.attrs[j] = &ko_attr.attr;

#define VDD_RES_RW_ATTRIB(_rail, ko_attr, j, _name) \
	ko_attr.attr.name = __stringify(_name); \
	ko_attr.attr.mode = 644; \
	ko_attr.show = vdd_rstr_reg_##_name##_show; \
	ko_attr.store = vdd_rstr_reg_##_name##_store; \
	sysfs_attr_init(&ko_attr.attr); \
	_rail.attr_gp.attrs[j] = &ko_attr.attr;

#define VDD_RSTR_ENABLE_FROM_ATTRIBS(attr) \
	(container_of(attr, struct vdd_rstr_enable, ko_attr));

#define VDD_RSTR_REG_VALUE_FROM_ATTRIBS(attr) \
	(container_of(attr, struct rail, value_attr));

#define VDD_RSTR_REG_LEVEL_FROM_ATTRIBS(attr) \
	(container_of(attr, struct rail, level_attr));

#define OCR_RW_ATTRIB(_rail, ko_attr, j, _name) \
	ko_attr.attr.name = __stringify(_name); \
	ko_attr.attr.mode = 644; \
	ko_attr.show = ocr_reg_##_name##_show; \
	ko_attr.store = ocr_reg_##_name##_store; \
	sysfs_attr_init(&ko_attr.attr); \
	_rail.attr_gp.attrs[j] = &ko_attr.attr;

#define PSM_RW_ATTRIB(_rail, ko_attr, j, _name) \
	ko_attr.attr.name = __stringify(_name); \
	ko_attr.attr.mode = 644; \
	ko_attr.show = psm_reg_##_name##_show; \
	ko_attr.store = psm_reg_##_name##_store; \
	sysfs_attr_init(&ko_attr.attr); \
	_rail.attr_gp.attrs[j] = &ko_attr.attr;

#define PSM_REG_MODE_FROM_ATTRIBS(attr) \
	(container_of(attr, struct psm_rail, mode_attr));

static int  msm_thermal_cpufreq_callback(struct notifier_block *nfb,
		unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;
	uint32_t max_freq_req = cpus[policy->cpu].limited_max_freq;
	uint32_t min_freq_req = cpus[policy->cpu].limited_min_freq;

	switch (event) {
	case CPUFREQ_INCOMPATIBLE:
		pr_debug("%s: mitigating cpu %d to freq max: %u min: %u\n",
		KBUILD_MODNAME, policy->cpu, max_freq_req, min_freq_req);

		cpufreq_verify_within_limits(policy, min_freq_req,
			max_freq_req);

		if (max_freq_req < min_freq_req)
			pr_err("Invalid frequency request Max:%u Min:%u\n",
				max_freq_req, min_freq_req);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block msm_thermal_cpufreq_notifier = {
	.notifier_call = msm_thermal_cpufreq_callback,
};

/* If freq table exists, then we can send freq request */
static int check_freq_table(void)
{
	int ret = 0;
	struct cpufreq_frequency_table *table = NULL;

	table = cpufreq_frequency_get_table(0);
	if (!table) {
		pr_debug("%s: error reading cpufreq table\n", __func__);
		return -EINVAL;
	}
	freq_table_get = 1;

	return ret;
}

static void update_cpu_freq(int cpu)
{
	if (cpu_online(cpu)) {
		if (cpufreq_update_policy(cpu))
			pr_err("Unable to update policy for cpu:%d\n", cpu);
	}
}

static int update_cpu_min_freq_all(uint32_t min)
{
	uint32_t cpu = 0;
	int ret = 0;

	if (!freq_table_get) {
		ret = check_freq_table();
		if (ret) {
			pr_err("%s:Fail to get freq table\n", KBUILD_MODNAME);
			return ret;
		}
	}
	/* If min is larger than allowed max */
	min = min(min, table[limit_idx_high].frequency);

	if (freq_mitigation_task) {
		min_freq_limit = min;
		complete(&freq_mitigation_complete);
	} else {
		get_online_cpus();
		for_each_possible_cpu(cpu) {
			cpus[cpu].limited_min_freq = min;
			update_cpu_freq(cpu);
		}
		put_online_cpus();
	}

	return ret;
}

static int vdd_restriction_apply_freq(struct rail *r, int level)
{
	int ret = 0;

	if (level == r->curr_level)
		return ret;

	/* level = -1: disable, level = 0,1,2..n: enable */
	if (level == -1) {
		ret = update_cpu_min_freq_all(r->min_level);
		if (ret)
			return ret;
		else
			r->curr_level = -1;
	} else if (level >= 0 && level < (r->num_levels)) {
		ret = update_cpu_min_freq_all(r->levels[level]);
		if (ret)
			return ret;
		else
			r->curr_level = level;
	} else {
		pr_err("level input:%d is not within range\n", level);
		return -EINVAL;
	}

	return ret;
}

static int vdd_restriction_apply_voltage(struct rail *r, int level)
{
	int ret = 0;

	if (r->reg == NULL) {
		pr_info("Do not have regulator handle:%s, can't apply vdd\n",
				r->name);
		return -EFAULT;
	}
	if (level == r->curr_level)
		return ret;

	/* level = -1: disable, level = 0,1,2..n: enable */
	if (level == -1) {
		ret = regulator_set_voltage(r->reg, r->min_level,
			r->levels[r->num_levels - 1]);
		if (!ret)
			r->curr_level = -1;
	} else if (level >= 0 && level < (r->num_levels)) {
		ret = regulator_set_voltage(r->reg, r->levels[level],
			r->levels[r->num_levels - 1]);
		if (!ret)
			r->curr_level = level;
	} else {
		pr_err("level input:%d is not within range\n", level);
		return -EINVAL;
	}

	return ret;
}

/* Setting all rails the same mode */
static int psm_set_mode_all(int mode)
{
	int i = 0;
	int fail_cnt = 0;
	int ret = 0;

	for (i = 0; i < psm_rails_cnt; i++) {
		if (psm_rails[i].mode != mode) {
			ret = rpm_regulator_set_mode(psm_rails[i].reg, mode);
			if (ret) {
				pr_err("Cannot set mode:%d for %s",
					mode, psm_rails[i].name);
				fail_cnt++;
			} else
				psm_rails[i].mode = mode;
		}
	}

	return fail_cnt ? (-EFAULT) : ret;
}

static int vdd_rstr_en_show(
	struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct vdd_rstr_enable *en = VDD_RSTR_ENABLE_FROM_ATTRIBS(attr);

	return snprintf(buf, PAGE_SIZE, "%d\n", en->enabled);
}

static ssize_t vdd_rstr_en_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	int i = 0;
	uint8_t en_cnt = 0;
	uint8_t dis_cnt = 0;
	uint32_t val = 0;
	struct kernel_param kp;
	struct vdd_rstr_enable *en = VDD_RSTR_ENABLE_FROM_ATTRIBS(attr);

	mutex_lock(&vdd_rstr_mutex);
	kp.arg = &val;
	ret = param_set_bool(buf, &kp);
	if (ret) {
		pr_err("Invalid input %s for enabled\n", buf);
		goto done_vdd_rstr_en;
	}

	if ((val == 0) && (en->enabled == 0))
		goto done_vdd_rstr_en;

	for (i = 0; i < rails_cnt; i++) {
		if (rails[i].freq_req == 1 && freq_table_get)
			ret = vdd_restriction_apply_freq(&rails[i],
					(val) ? 0 : -1);
		else
			ret = vdd_restriction_apply_voltage(&rails[i],
			(val) ? 0 : -1);

		/*
		 * Even if fail to set one rail, still try to set the
		 * others. Continue the loop
		 */
		if (ret)
			pr_err("Set vdd restriction for %s failed\n",
					rails[i].name);
		else {
			if (val)
				en_cnt++;
			else
				dis_cnt++;
		}
	}
	/* As long as one rail is enabled, vdd rstr is enabled */
	if (val && en_cnt)
		en->enabled = 1;
	else if (!val && (dis_cnt == rails_cnt))
		en->enabled = 0;

done_vdd_rstr_en:
	mutex_unlock(&vdd_rstr_mutex);
	return count;
}

static struct vdd_rstr_enable vdd_rstr_en = {
	.ko_attr.attr.name = __stringify(enabled),
	.ko_attr.attr.mode = 644,
	.ko_attr.show = vdd_rstr_en_show,
	.ko_attr.store = vdd_rstr_en_store,
	.enabled = 1,
};

static struct attribute *vdd_rstr_en_attribs[] = {
	&vdd_rstr_en.ko_attr.attr,
	NULL,
};

static struct attribute_group vdd_rstr_en_attribs_gp = {
	.attrs  = vdd_rstr_en_attribs,
};

static int vdd_rstr_reg_value_show(
	struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int val = 0;
	struct rail *reg = VDD_RSTR_REG_VALUE_FROM_ATTRIBS(attr);
	/* -1:disabled, -2:fail to get regualtor handle */
	if (reg->curr_level < 0)
		val = reg->curr_level;
	else
		val = reg->levels[reg->curr_level];

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static int vdd_rstr_reg_level_show(
	struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct rail *reg = VDD_RSTR_REG_LEVEL_FROM_ATTRIBS(attr);
	return snprintf(buf, PAGE_SIZE, "%d\n", reg->curr_level);
}

static ssize_t vdd_rstr_reg_level_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	int val = 0;

	struct rail *reg = VDD_RSTR_REG_LEVEL_FROM_ATTRIBS(attr);

	mutex_lock(&vdd_rstr_mutex);
	if (vdd_rstr_en.enabled == 0)
		goto done_store_level;

	ret = kstrtouint(buf, 10, &val);
	if (ret) {
		pr_err("Invalid input %s for level\n", buf);
		goto done_store_level;
	}

	if (val < 0 || val > reg->num_levels - 1) {
		pr_err(" Invalid number %d for level\n", val);
		goto done_store_level;
	}

	if (val != reg->curr_level) {
		if (reg->freq_req == 1 && freq_table_get)
			update_cpu_min_freq_all(reg->levels[val]);
		else {
			ret = vdd_restriction_apply_voltage(reg, val);
			if (ret) {
				pr_err( \
				"Set vdd restriction for regulator %s failed\n",
				reg->name);
				goto done_store_level;
			}
		}
		reg->curr_level = val;
	}

done_store_level:
	mutex_unlock(&vdd_rstr_mutex);
	return count;
}

static int request_optimum_current(struct psm_rail *rail, enum ocr_request req)
{
	int ret = 0;

	if ((!rail) || (req >= OPTIMUM_CURRENT_NR) ||
		(req < 0)) {
		pr_err("%s:%s Invalid input\n", KBUILD_MODNAME, __func__);
		ret = -EINVAL;
		goto request_ocr_exit;
	}

	ret = regulator_set_optimum_mode(rail->phase_reg,
		(req == OPTIMUM_CURRENT_MAX) ? MAX_CURRENT_UA : 0);
	if (ret < 0) {
		pr_err("%s: Optimum current request failed\n", KBUILD_MODNAME);
		goto request_ocr_exit;
	}
	ret = 0; /*regulator_set_optimum_mode returns the mode on success*/
	pr_debug("%s: Requested optimum current mode: %d\n",
		KBUILD_MODNAME, req);

request_ocr_exit:
	return ret;
}

static int ocr_set_mode_all(enum ocr_request req)
{
	int ret = 0, i;

	for (i = 0; i < ocr_rail_cnt; i++) {
		if (ocr_rails[i].mode == req)
			continue;
		ret = request_optimum_current(&ocr_rails[i], req);
		if (ret)
			goto ocr_set_mode_exit;
		ocr_rails[i].mode = req;
	}

ocr_set_mode_exit:
	return ret;
}

static int ocr_reg_mode_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	struct psm_rail *reg = PSM_REG_MODE_FROM_ATTRIBS(attr);
	return snprintf(buf, PAGE_SIZE, "%d\n", reg->mode);
}

static ssize_t ocr_reg_mode_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	int val = 0;
	struct psm_rail *reg = PSM_REG_MODE_FROM_ATTRIBS(attr);

	if (!ocr_enabled)
		return count;

	mutex_lock(&ocr_mutex);
	ret = kstrtoint(buf, 10, &val);
	if (ret) {
		pr_err("%s: Invalid input %s for mode\n",
			KBUILD_MODNAME, buf);
		goto done_ocr_store;
	}

	if ((val != OPTIMUM_CURRENT_MAX) &&
		(val != OPTIMUM_CURRENT_MIN)) {
		pr_err("%s: Invalid value %d for mode\n",
			KBUILD_MODNAME, val);
		goto done_ocr_store;
	}

	if (val != reg->mode) {
		ret = request_optimum_current(reg, val);
		if (ret)
			goto done_ocr_store;
		reg->mode = val;
	}

done_ocr_store:
	mutex_unlock(&ocr_mutex);
	return count;
}

static int psm_reg_mode_show(
	struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct psm_rail *reg = PSM_REG_MODE_FROM_ATTRIBS(attr);
	return snprintf(buf, PAGE_SIZE, "%d\n", reg->mode);
}

static ssize_t psm_reg_mode_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	int val = 0;
	struct psm_rail *reg = PSM_REG_MODE_FROM_ATTRIBS(attr);

	mutex_lock(&psm_mutex);
	ret = kstrtoint(buf, 10, &val);
	if (ret) {
		pr_err("Invalid input %s for mode\n", buf);
		goto done_psm_store;
	}

	if ((val != PMIC_PWM_MODE) && (val != PMIC_AUTO_MODE)) {
		pr_err(" Invalid number %d for mode\n", val);
		goto done_psm_store;
	}

	if (val != reg->mode) {
		ret = rpm_regulator_set_mode(reg->reg, val);
		if (ret) {
			pr_err( \
			"Fail to set PMIC SW Mode:%d for %s\n",
			val, reg->name);
			goto done_psm_store;
		}
		reg->mode = val;
	}

done_psm_store:
	mutex_unlock(&psm_mutex);
	return count;
}

static int check_sensor_id(int sensor_id)
{
	int i = 0;
	bool hw_id_found;
	int ret = 0;

	for (i = 0; i < max_tsens_num; i++) {
		if (sensor_id == tsens_id_map[i]) {
			hw_id_found = true;
			break;
		}
	}
	if (!hw_id_found) {
		pr_err("%s: Invalid sensor hw id :%d\n", __func__, sensor_id);
		return -EINVAL;
	}

	return ret;
}

static int create_sensor_id_map(void)
{
	int i = 0;
	int ret = 0;

	tsens_id_map = kzalloc(sizeof(int) * max_tsens_num,
			GFP_KERNEL);
	if (!tsens_id_map) {
		pr_err("%s: Cannot allocate memory for tsens_id_map\n",
				__func__);
		return -ENOMEM;
	}

	for (i = 0; i < max_tsens_num; i++) {
		ret = tsens_get_hw_id_mapping(i, &tsens_id_map[i]);
		/* If return -ENXIO, hw_id is default in sequence */
		if (ret) {
			if (ret == -ENXIO) {
				tsens_id_map[i] = i;
				ret = 0;
			} else {
				pr_err( \
				"%s: Failed to get hw id for sw id %d\n",
				__func__, i);
				goto fail;
			}
		}
	}

	return ret;
fail:
	kfree(tsens_id_map);
	return ret;
}

/* 1:enable, 0:disable */
static int vdd_restriction_apply_all(int en)
{
	int i = 0;
	int en_cnt = 0;
	int dis_cnt = 0;
	int fail_cnt = 0;
	int ret = 0;

	for (i = 0; i < rails_cnt; i++) {
		if (rails[i].freq_req == 1 && freq_table_get)
			ret = vdd_restriction_apply_freq(&rails[i],
					en ? 0 : -1);
		else
			ret = vdd_restriction_apply_voltage(&rails[i],
					en ? 0 : -1);
		if (ret) {
			pr_err("Cannot set voltage for %s", rails[i].name);
			fail_cnt++;
		} else {
			if (en)
				en_cnt++;
			else
				dis_cnt++;
		}
	}

	/* As long as one rail is enabled, vdd rstr is enabled */
	if (en && en_cnt)
		vdd_rstr_en.enabled = 1;
	else if (!en && (dis_cnt == rails_cnt))
		vdd_rstr_en.enabled = 0;

	/*
	 * Check fail_cnt again to make sure all of the rails are applied
	 * restriction successfully or not
	 */
	if (fail_cnt)
		return -EFAULT;
	return ret;
}

static int msm_thermal_get_freq_table(void)
{
	int ret = 0;
	int i = 0;

	table = cpufreq_frequency_get_table(0);
	if (table == NULL) {
		pr_debug("%s: error reading cpufreq table\n", KBUILD_MODNAME);
		ret = -EINVAL;
		goto fail;
	}

	while (table[i].frequency != CPUFREQ_TABLE_END)
		i++;
//#ifdef CONFIG_SEC_PM
//	limit_idx_low = 7;
//#else
	limit_idx_low = 0;
//#endif
	limit_idx_high = limit_idx = i - 1;
	BUG_ON(limit_idx_high <= 0 || limit_idx_high <= limit_idx_low);
fail:
	return ret;
}

static int set_and_activate_threshold(uint32_t sensor_id,
	struct sensor_threshold *threshold)
{
	int ret = 0;

	ret = sensor_set_trip(sensor_id, threshold);
	if (ret != 0) {
		pr_err("%s: Error in setting trip %d\n",
			KBUILD_MODNAME, threshold->trip);
		goto set_done;
	}

	ret = sensor_activate_trip(sensor_id, threshold, true);
	if (ret != 0) {
		pr_err("%s: Error in enabling trip %d\n",
			KBUILD_MODNAME, threshold->trip);
		goto set_done;
	}

set_done:
	return ret;
}

static int set_threshold(uint32_t sensor_id,
	struct sensor_threshold *threshold)
{
	struct tsens_device tsens_dev;
	int i = 0, ret = 0;
	long temp;

	if ((!threshold) || check_sensor_id(sensor_id)) {
		pr_err("%s: Invalid input\n", KBUILD_MODNAME);
		ret = -EINVAL;
		goto set_threshold_exit;
	}

	tsens_dev.sensor_num = sensor_id;
	ret = tsens_get_temp(&tsens_dev, &temp);
	if (ret) {
		pr_err("%s: Unable to read TSENS sensor %d\n",
			KBUILD_MODNAME, tsens_dev.sensor_num);
		goto set_threshold_exit;
	}
	while (i < MAX_THRESHOLD) {
		switch (threshold[i].trip) {
		case THERMAL_TRIP_CONFIGURABLE_HI:
			if (threshold[i].temp >= temp) {
				ret = set_and_activate_threshold(sensor_id,
					&threshold[i]);
				if (ret)
					goto set_threshold_exit;
			}
			break;
		case THERMAL_TRIP_CONFIGURABLE_LOW:
			if (threshold[i].temp <= temp) {
				ret = set_and_activate_threshold(sensor_id,
					&threshold[i]);
				if (ret)
					goto set_threshold_exit;
			}
			break;
		default:
			break;
		}
		i++;
	}
set_threshold_exit:
	return ret;
}

#ifdef CONFIG_SMP
static void __ref do_core_control(long temp)
{
	int i = 0;
	int ret = 0;

	if (!core_control_enabled)
		return;

	mutex_lock(&core_control_mutex);
	if (msm_thermal_info.core_control_mask &&
		temp >= msm_thermal_info.core_limit_temp_degC) {
		for (i = num_possible_cpus(); i > 0; i--) {
			if (!(msm_thermal_info.core_control_mask & BIT(i)))
				continue;
			if (cpus_offlined & BIT(i) && !cpu_online(i))
				continue;
			pr_info("%s: Set Offline: CPU%d Temp: %ld\n",
					KBUILD_MODNAME, i, temp);
			ret = cpu_down(i);
			if (ret)
				pr_err("%s: Error %d offline core %d\n",
					KBUILD_MODNAME, ret, i);
			cpus_offlined |= BIT(i);
			break;
		}
	} else if (msm_thermal_info.core_control_mask && cpus_offlined &&
		temp <= (msm_thermal_info.core_limit_temp_degC -
			msm_thermal_info.core_temp_hysteresis_degC)) {
		for (i = 0; i < num_possible_cpus(); i++) {
			if (!(cpus_offlined & BIT(i)))
				continue;
			cpus_offlined &= ~BIT(i);
			pr_info("%s: Allow Online CPU%d Temp: %ld\n",
					KBUILD_MODNAME, i, temp);
			/*
			 * If this core is already online, then bring up the
			 * next offlined core.
			 */
			if (cpu_online(i))
				continue;
			ret = cpu_up(i);
			if (ret)
				pr_err("%s: Error %d online core %d\n",
						KBUILD_MODNAME, ret, i);
			break;
		}
	}
	mutex_unlock(&core_control_mutex);
}
/* Call with core_control_mutex locked */
static int __ref update_offline_cores(int val)
{
	uint32_t cpu = 0;
	int ret = 0;

	if (!core_control_enabled)
		return 0;

	cpus_offlined = msm_thermal_info.core_control_mask & val;

	for_each_possible_cpu(cpu) {
		if (!(cpus_offlined & BIT(cpu)))
			continue;
		if (!cpu_online(cpu))
			continue;
		ret = cpu_down(cpu);
		if (ret)
			pr_err("%s: Unable to offline cpu%d\n",
				KBUILD_MODNAME, cpu);
	}
	return ret;
}

static __ref int do_hotplug(void *data)
{
	int ret = 0;
	uint32_t cpu = 0, mask = 0;

	if (!core_control_enabled)
		return -EINVAL;

	while (!kthread_should_stop()) {
		wait_for_completion(&hotplug_notify_complete);
		INIT_COMPLETION(hotplug_notify_complete);
		mask = 0;

		mutex_lock(&core_control_mutex);
		for_each_possible_cpu(cpu) {
			if (hotplug_enabled &&
				cpus[cpu].hotplug_thresh_clear) {
				set_threshold(cpus[cpu].sensor_id,
				&cpus[cpu].threshold[HOTPLUG_THRESHOLD_HIGH]);

				cpus[cpu].hotplug_thresh_clear = false;
			}
			if (cpus[cpu].offline || cpus[cpu].user_offline)
				mask |= BIT(cpu);
		}
		if (mask != cpus_offlined)
			update_offline_cores(mask);
		mutex_unlock(&core_control_mutex);
		sysfs_notify(cc_kobj, NULL, "cpus_offlined");
	}

	return ret;
}
#else
static void do_core_control(long temp)
{
	return;
}

static __ref int do_hotplug(void *data)
{
	return 0;
}
#endif

static int do_ocr(void)
{
	struct tsens_device tsens_dev;
	long temp = 0;
	int ret = 0;
	int i = 0, j = 0;
	int auto_cnt = 0;

	if (!ocr_enabled)
		return ret;

	mutex_lock(&ocr_mutex);
	for (i = 0; i < max_tsens_num; i++) {
		tsens_dev.sensor_num = tsens_id_map[i];
		ret = tsens_get_temp(&tsens_dev, &temp);
		if (ret) {
			pr_debug("%s: Unable to read TSENS sensor %d\n",
					__func__, tsens_dev.sensor_num);
			auto_cnt++;
			continue;
		}

		if (temp > msm_thermal_info.ocr_temp_degC) {
			if (ocr_rails[0].init != OPTIMUM_CURRENT_NR)
				for (j = 0; j < ocr_rail_cnt; j++)
					ocr_rails[j].init = OPTIMUM_CURRENT_NR;
			ret = ocr_set_mode_all(OPTIMUM_CURRENT_MAX);
			if (ret)
				pr_err("Error setting max optimum current\n");
			goto do_ocr_exit;
		} else if (temp <= (msm_thermal_info.ocr_temp_degC -
			msm_thermal_info.ocr_temp_hyst_degC))
			auto_cnt++;
	}

	if (auto_cnt == max_tsens_num ||
		ocr_rails[0].init != OPTIMUM_CURRENT_NR) {
		/* 'init' not equal to OPTIMUM_CURRENT_NR means this is the
		** first polling iteration after device probe. During first
		** iteration, if temperature is less than the set point, clear
		** the max current request made and reset the 'init'.
		*/
		if (ocr_rails[0].init != OPTIMUM_CURRENT_NR)
			for (j = 0; j < ocr_rail_cnt; j++)
				ocr_rails[j].init = OPTIMUM_CURRENT_NR;
		ret = ocr_set_mode_all(OPTIMUM_CURRENT_MIN);
		if (ret) {
			pr_err("Error setting min optimum current\n");
			goto do_ocr_exit;
		}
	}

do_ocr_exit:
	mutex_unlock(&ocr_mutex);
	return ret;
}

static int do_vdd_restriction(void)
{
	struct tsens_device tsens_dev;
	long temp = 0;
	int ret = 0;
	int i = 0;
	int dis_cnt = 0;

	if (!vdd_rstr_enabled)
		return ret;

	if (usefreq && !freq_table_get) {
		if (check_freq_table())
			return ret;
	}

	mutex_lock(&vdd_rstr_mutex);
	for (i = 0; i < max_tsens_num; i++) {
		tsens_dev.sensor_num = tsens_id_map[i];
		ret = tsens_get_temp(&tsens_dev, &temp);
		if (ret) {
			pr_debug("%s: Unable to read TSENS sensor %d\n",
					__func__, tsens_dev.sensor_num);
			dis_cnt++;
			continue;
		}
		if (temp <=  msm_thermal_info.vdd_rstr_temp_degC) {
			ret = vdd_restriction_apply_all(1);
			if (ret) {
				pr_err( \
				"Enable vdd rstr votlage for all failed\n");
				goto exit;
			}
			goto exit;
		} else if (temp > msm_thermal_info.vdd_rstr_temp_hyst_degC)
			dis_cnt++;
	}
	if (dis_cnt == max_tsens_num) {
		ret = vdd_restriction_apply_all(0);
		if (ret) {
			pr_err("Disable vdd rstr votlage for all failed\n");
			goto exit;
		}
	}
exit:
	mutex_unlock(&vdd_rstr_mutex);
	return ret;
}

static int do_psm(void)
{
	struct tsens_device tsens_dev;
	long temp = 0;
	int ret = 0;
	int i = 0;
	int auto_cnt = 0;

	mutex_lock(&psm_mutex);
	for (i = 0; i < max_tsens_num; i++) {
		tsens_dev.sensor_num = tsens_id_map[i];
		ret = tsens_get_temp(&tsens_dev, &temp);
		if (ret) {
			pr_debug("%s: Unable to read TSENS sensor %d\n",
					__func__, tsens_dev.sensor_num);
			auto_cnt++;
			continue;
		}

		/*
		 * As long as one sensor is above the threshold, set PWM mode
		 * on all rails, and loop stops. Set auto mode when all rails
		 * are below thershold
		 */
		if (temp >  msm_thermal_info.psm_temp_degC) {
			ret = psm_set_mode_all(PMIC_PWM_MODE);
			if (ret) {
				pr_err("Set pwm mode for all failed\n");
				goto exit;
			}
			break;
		} else if (temp <= msm_thermal_info.psm_temp_hyst_degC)
			auto_cnt++;
	}

	if (auto_cnt == max_tsens_num) {
		ret = psm_set_mode_all(PMIC_AUTO_MODE);
		if (ret) {
			pr_err("Set auto mode for all failed\n");
			goto exit;
		}
	}

exit:
	mutex_unlock(&psm_mutex);
	return ret;
}

static void __ref do_freq_control(long temp)
{
	uint32_t cpu = 0;
	uint32_t max_freq = cpus[cpu].limited_max_freq;

	if (temp >= msm_thermal_info.limit_temp_degC) {
		if (limit_idx == limit_idx_low)
			return;

		limit_idx -= msm_thermal_info.bootup_freq_step;
		if (limit_idx < limit_idx_low)
			limit_idx = limit_idx_low;
		max_freq = table[limit_idx].frequency;

#ifdef CONFIG_SEC_PM_DEBUG
		pr_info("%s: down Limit=%d Temp: %ld\n",
				KBUILD_MODNAME, limit_idx, temp);
#endif
	} else if (temp < msm_thermal_info.limit_temp_degC -
		 msm_thermal_info.temp_hysteresis_degC) {
		if (limit_idx == limit_idx_high)
			return;

		limit_idx += msm_thermal_info.bootup_freq_step;
		if (limit_idx >= limit_idx_high) {
			limit_idx = limit_idx_high;
			max_freq = UINT_MAX;
		} else
			max_freq = table[limit_idx].frequency;

#ifdef CONFIG_SEC_PM_DEBUG
		pr_info("%s: up Limit=%d Temp: %ld\n",
				KBUILD_MODNAME, limit_idx, temp);
#endif
	}

	if (max_freq == cpus[cpu].limited_max_freq)
		return;

	/* Update new limits */
	get_online_cpus();
	for_each_possible_cpu(cpu) {
		if (!(msm_thermal_info.bootup_freq_control_mask & BIT(cpu)))
			continue;
		cpus[cpu].limited_max_freq = max_freq;
		update_cpu_freq(cpu);
	}
	put_online_cpus();
}

static void __ref check_temp(struct work_struct *work)
{
	static int limit_init;
	struct tsens_device tsens_dev;
	long temp = 0;
	int ret = 0;

	tsens_dev.sensor_num = msm_thermal_info.sensor_id;
	ret = tsens_get_temp(&tsens_dev, &temp);
	if (ret) {
		pr_debug("%s: Unable to read TSENS sensor %d\n",
				KBUILD_MODNAME, tsens_dev.sensor_num);
		goto reschedule;
	}

	if (!limit_init) {
		ret = msm_thermal_get_freq_table();
		if (ret)
			goto reschedule;
		else
			limit_init = 1;
	}

	do_core_control(temp);
	do_vdd_restriction();
	do_psm();
	do_ocr();
	do_freq_control(temp);

reschedule:
	if (enabled)
		schedule_delayed_work(&check_temp_work,
				msecs_to_jiffies(msm_thermal_info.poll_ms));
}


static void __ref msm_therm_temp_log(struct work_struct *work)
{

	struct tsens_device tsens_dev;
	long temp =  0;
	int i, added = 0, ret = 0;
	uint32_t max_sensors = 0;
	char buffer[500];

	if(!tsens_get_max_sensor_num(&max_sensors))
	{
		pr_info( "Debug Temp for Sensor: ");
		for(i=0;i<max_sensors;i++)
		{
			tsens_dev.sensor_num = i;
			tsens_get_temp(&tsens_dev, &temp);
			ret = sprintf(buffer + added, "(%d --- %ld)", i, temp);
			added += ret;						
		}
		pr_info("%s", buffer);
	}
	schedule_delayed_work(&temp_log_work,
				HZ*5); //For every 5 seconds log the temperature values of all the msm thermistors.
}

static int __ref msm_thermal_cpu_callback(struct notifier_block *nfb,
		unsigned long action, void *hcpu)
{
	uint32_t cpu = (uint32_t)hcpu;

	if (action == CPU_UP_PREPARE || action == CPU_UP_PREPARE_FROZEN) {
		if (core_control_enabled &&
			(msm_thermal_info.core_control_mask & BIT(cpu)) &&
			(cpus_offlined & BIT(cpu))) {
			pr_debug(
			"%s: Preventing cpu%d from coming online.\n",
				KBUILD_MODNAME, cpu);
			return NOTIFY_BAD;
		}
	}


	return NOTIFY_OK;
}

static struct notifier_block __refdata msm_thermal_cpu_notifier = {
	.notifier_call = msm_thermal_cpu_callback,
};

static void thermal_rtc_setup(void)
{
	ktime_t wakeup_time;
	ktime_t curr_time;

	curr_time = android_alarm_get_elapsed_realtime();
	wakeup_time = ktime_add_us(curr_time,
			(wakeup_ms * USEC_PER_MSEC));
	android_alarm_start_range(&thermal_rtc, wakeup_time,
			wakeup_time);
	pr_debug("%s: Current Time: %ld %ld, Alarm set to: %ld %ld\n",
			KBUILD_MODNAME,
			ktime_to_timeval(curr_time).tv_sec,
			ktime_to_timeval(curr_time).tv_usec,
			ktime_to_timeval(wakeup_time).tv_sec,
			ktime_to_timeval(wakeup_time).tv_usec);

}

static void timer_work_fn(struct work_struct *work)
{
	sysfs_notify(tt_kobj, NULL, "wakeup_ms");
}

static void thermal_rtc_callback(struct alarm *al)
{
	struct timeval ts;
	ts = ktime_to_timeval(android_alarm_get_elapsed_realtime());
	schedule_work(&timer_work);
	pr_debug("%s: Time on alarm expiry: %ld %ld\n", KBUILD_MODNAME,
			ts.tv_sec, ts.tv_usec);
}

static int hotplug_notify(enum thermal_trip_type type, int temp, void *data)
{
	struct cpu_info *cpu_node = (struct cpu_info *)data;

	pr_info("%s: %s reach temp threshold: %d\n", KBUILD_MODNAME,
			cpu_node->sensor_type, temp);

	if (!(msm_thermal_info.core_control_mask & BIT(cpu_node->cpu)))
		return 0;
	switch (type) {
	case THERMAL_TRIP_CONFIGURABLE_HI:
		if (!(cpu_node->offline))
			cpu_node->offline = 1;
		break;
	case THERMAL_TRIP_CONFIGURABLE_LOW:
		if (cpu_node->offline)
			cpu_node->offline = 0;
		break;
	default:
		break;
	}
	if (hotplug_task) {
		cpu_node->hotplug_thresh_clear = true;
		complete(&hotplug_notify_complete);
	} else {
		pr_err("%s: Hotplug task is not initialized\n", KBUILD_MODNAME);
	}
	return 0;
}
/* Adjust cpus offlined bit based on temperature reading. */
static int hotplug_init_cpu_offlined(void)
{
	struct tsens_device tsens_dev;
	long temp = 0;
	uint32_t cpu = 0;

	if (!hotplug_enabled)
		return 0;

	mutex_lock(&core_control_mutex);
	for_each_possible_cpu(cpu) {
		if (!(msm_thermal_info.core_control_mask & BIT(cpus[cpu].cpu)))
			continue;
		tsens_dev.sensor_num = cpus[cpu].sensor_id;
		if (tsens_get_temp(&tsens_dev, &temp)) {
			pr_err("%s: Unable to read TSENS sensor %d\n",
				KBUILD_MODNAME, tsens_dev.sensor_num);
			mutex_unlock(&core_control_mutex);
			return -EINVAL;
		}

		if (temp >= msm_thermal_info.hotplug_temp_degC)
			cpus[cpu].offline = 1;
		else if (temp <= (msm_thermal_info.hotplug_temp_degC -
			msm_thermal_info.hotplug_temp_hysteresis_degC))
			cpus[cpu].offline = 0;
	}
	mutex_unlock(&core_control_mutex);

	if (hotplug_task)
		complete(&hotplug_notify_complete);
	else {
		pr_err("%s: Hotplug task is not initialized\n",
					KBUILD_MODNAME);
		return -EINVAL;
	}
	return 0;
}

static void hotplug_init(void)
{
	uint32_t cpu = 0;
	struct sensor_threshold *hi_thresh = NULL, *low_thresh = NULL;

	if (hotplug_task)
		return;

	if (!hotplug_enabled)
		goto init_kthread;

	for_each_possible_cpu(cpu) {
		cpus[cpu].sensor_id =
			sensor_get_id((char *)cpus[cpu].sensor_type);
		if (!(msm_thermal_info.core_control_mask & BIT(cpus[cpu].cpu)))
			continue;

		hi_thresh = &cpus[cpu].threshold[HOTPLUG_THRESHOLD_HIGH];
		low_thresh = &cpus[cpu].threshold[HOTPLUG_THRESHOLD_LOW];
		hi_thresh->temp = msm_thermal_info.hotplug_temp_degC;
		hi_thresh->trip = THERMAL_TRIP_CONFIGURABLE_HI;
		low_thresh->temp = msm_thermal_info.hotplug_temp_degC -
				msm_thermal_info.hotplug_temp_hysteresis_degC;
		low_thresh->trip = THERMAL_TRIP_CONFIGURABLE_LOW;
		hi_thresh->notify = low_thresh->notify = hotplug_notify;
		hi_thresh->data = low_thresh->data = (void *)&cpus[cpu];

		set_threshold(cpus[cpu].sensor_id, hi_thresh);
	}
init_kthread:
	init_completion(&hotplug_notify_complete);
	hotplug_task = kthread_run(do_hotplug, NULL, "msm_thermal:hotplug");
	if (IS_ERR(hotplug_task)) {
		pr_err("%s: Failed to create do_hotplug thread\n",
				KBUILD_MODNAME);
		return;
	}
	/*
	 * Adjust cpus offlined bit when hotplug intitializes so that the new
	 * cpus offlined state is based on hotplug threshold range
	 */
	if (hotplug_init_cpu_offlined())
		kthread_stop(hotplug_task);
}

static __ref int do_freq_mitigation(void *data)
{
	int ret = 0;
	uint32_t cpu = 0, max_freq_req = 0, min_freq_req = 0;

	while (!kthread_should_stop()) {
		wait_for_completion(&freq_mitigation_complete);
		INIT_COMPLETION(freq_mitigation_complete);

		get_online_cpus();
		for_each_possible_cpu(cpu) {
			max_freq_req = (cpus[cpu].max_freq) ?
					msm_thermal_info.freq_limit :
					UINT_MAX;
			max_freq_req = min(max_freq_req,
					cpus[cpu].user_max_freq);

			min_freq_req = max(min_freq_limit,
					cpus[cpu].user_min_freq);

			if ((max_freq_req == cpus[cpu].limited_max_freq)
				&& (min_freq_req ==
				cpus[cpu].limited_min_freq))
				goto reset_threshold;

			cpus[cpu].limited_max_freq = max_freq_req;
			cpus[cpu].limited_min_freq = min_freq_req;
			update_cpu_freq(cpu);
reset_threshold:
			if (freq_mitigation_enabled &&
				cpus[cpu].freq_thresh_clear) {
				set_threshold(cpus[cpu].sensor_id,
				&cpus[cpu].threshold[FREQ_THRESHOLD_HIGH]);

				cpus[cpu].freq_thresh_clear = false;
			}
		}
		put_online_cpus();
	}
	return ret;
}

static int freq_mitigation_notify(enum thermal_trip_type type,
	int temp, void *data)
{
	struct cpu_info *cpu_node = (struct cpu_info *) data;

	pr_debug("%s: %s reached temp threshold: %d\n", KBUILD_MODNAME,
		cpu_node->sensor_type, temp);

	if (!(msm_thermal_info.freq_mitig_control_mask &
		BIT(cpu_node->cpu)))
		return 0;

	switch (type) {
	case THERMAL_TRIP_CONFIGURABLE_HI:
		if (!cpu_node->max_freq) {
			pr_info("%s: Mitigating cpu %d frequency to %d\n",
				KBUILD_MODNAME, cpu_node->cpu,
				msm_thermal_info.freq_limit);

			cpu_node->max_freq = true;
		}
		break;
	case THERMAL_TRIP_CONFIGURABLE_LOW:
		if (cpu_node->max_freq) {
			pr_info("%s: Removing frequency mitigation for cpu%d\n",
				KBUILD_MODNAME, cpu_node->cpu);

			cpu_node->max_freq = false;
		}
		break;
	default:
		break;
	}

	if (freq_mitigation_task) {
		cpu_node->freq_thresh_clear = true;
		complete(&freq_mitigation_complete);
	} else {
		pr_err("%s: Frequency mitigation task is not initialized\n",
			KBUILD_MODNAME);
	}

	return 0;
}

static void freq_mitigation_init(void)
{
	uint32_t cpu = 0;
	struct sensor_threshold *hi_thresh = NULL, *low_thresh = NULL;

	if (freq_mitigation_task)
		return;
	if (!freq_mitigation_enabled)
		goto init_freq_thread;

	for_each_possible_cpu(cpu) {
		if (!(msm_thermal_info.freq_mitig_control_mask & BIT(cpu)))
			continue;
		hi_thresh = &cpus[cpu].threshold[FREQ_THRESHOLD_HIGH];
		low_thresh = &cpus[cpu].threshold[FREQ_THRESHOLD_LOW];

		hi_thresh->temp = msm_thermal_info.freq_mitig_temp_degc;
		hi_thresh->trip = THERMAL_TRIP_CONFIGURABLE_HI;
		low_thresh->temp = msm_thermal_info.freq_mitig_temp_degc -
			msm_thermal_info.freq_mitig_temp_hysteresis_degc;
		low_thresh->trip = THERMAL_TRIP_CONFIGURABLE_LOW;
		hi_thresh->notify = low_thresh->notify =
			freq_mitigation_notify;
		hi_thresh->data = low_thresh->data = (void *)&cpus[cpu];

		set_threshold(cpus[cpu].sensor_id, hi_thresh);
	}
init_freq_thread:
	init_completion(&freq_mitigation_complete);
	freq_mitigation_task = kthread_run(do_freq_mitigation, NULL,
		"msm_thermal:freq_mitig");

	if (IS_ERR(freq_mitigation_task)) {
		pr_err("%s: Failed to create frequency mitigation thread\n",
				KBUILD_MODNAME);
		return;
	}
}

int msm_thermal_set_frequency(uint32_t cpu, uint32_t freq, bool is_max)
{
	int ret = 0;

	if (cpu >= num_possible_cpus()) {
		pr_err("%s: Invalid input\n", KBUILD_MODNAME);
		ret = -EINVAL;
		goto set_freq_exit;
	}

	if (is_max) {
		if (cpus[cpu].user_max_freq == freq)
			goto set_freq_exit;

		cpus[cpu].user_max_freq = freq;
	} else {
		if (cpus[cpu].user_min_freq == freq)
			goto set_freq_exit;

		cpus[cpu].user_min_freq = freq;
	}

	if (freq_mitigation_task) {
		complete(&freq_mitigation_complete);
	} else {
		pr_err("%s: Frequency mitigation task is not initialized\n",
			KBUILD_MODNAME);
		ret = -ESRCH;
		goto set_freq_exit;
	}

set_freq_exit:
	return ret;
}

/*
 * We will reset the cpu frequencies limits here. The core online/offline
 * status will be carried over to the process stopping the msm_thermal, as
 * we dont want to online a core and bring in the thermal issues.
 */
static void __ref disable_msm_thermal(void)
{
	uint32_t cpu = 0;

	/* make sure check_temp is no longer running */
	/* kor_ts@sec
	 * flush_scheduled_work () should be avoided.
	 */
	cancel_delayed_work_sync(&check_temp_work);
	/*
	cancel_delayed_work(&check_temp_work);
	flush_scheduled_work();
	*/

	get_online_cpus();
	for_each_possible_cpu(cpu) {
		if (cpus[cpu].limited_max_freq == UINT_MAX &&
			cpus[cpu].limited_min_freq == 0)
			continue;
		cpus[cpu].limited_max_freq = UINT_MAX;
		cpus[cpu].limited_min_freq = 0;
		update_cpu_freq(cpu);
	}
	put_online_cpus();
}

static int __ref set_enabled(const char *val, const struct kernel_param *kp)
{
	int ret = 0;

	ret = param_set_bool(val, kp);
	if (!enabled) {
		disable_msm_thermal();
		hotplug_init();
		freq_mitigation_init();
	} else
		pr_info("%s: no action for enabled = %d\n",
			KBUILD_MODNAME, enabled);

	pr_info("%s: enabled = %d\n", KBUILD_MODNAME, enabled);

	return ret;
}

static struct kernel_param_ops module_ops = {
	.set = set_enabled,
	.get = param_get_bool,
};

module_param_cb(enabled, &module_ops, &enabled, 0644);
MODULE_PARM_DESC(enabled, "enforce thermal limit on cpu");

static ssize_t show_cc_enabled(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", core_control_enabled);
}

static ssize_t __ref store_cc_enabled(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	int val = 0;

	ret = kstrtoint(buf, 10, &val);
	if (ret) {
		pr_err("%s: Invalid input %s\n", KBUILD_MODNAME, buf);
		goto done_store_cc;
	}

	if (core_control_enabled == !!val)
		goto done_store_cc;

	core_control_enabled = !!val;
	if (core_control_enabled) {
		pr_info("%s: Core control enabled\n", KBUILD_MODNAME);
		register_cpu_notifier(&msm_thermal_cpu_notifier);
		if (hotplug_task)
			complete(&hotplug_notify_complete);
		else
			pr_err("%s: Hotplug task is not initialized\n",
					KBUILD_MODNAME);
	} else {
		pr_info("%s: Core control disabled\n", KBUILD_MODNAME);
		unregister_cpu_notifier(&msm_thermal_cpu_notifier);
	}

done_store_cc:
	return count;
}

static ssize_t show_cpus_offlined(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", cpus_offlined);
}

static ssize_t __ref store_cpus_offlined(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	uint32_t val = 0;
	uint32_t cpu;

	mutex_lock(&core_control_mutex);
	ret = kstrtouint(buf, 10, &val);
	if (ret) {
		pr_err("%s: Invalid input %s\n", KBUILD_MODNAME, buf);
		goto done_cc;
	}

	if (enabled) {
		pr_err("%s: Ignoring request; polling thread is enabled.\n",
				KBUILD_MODNAME);
		goto done_cc;
	}

	for_each_possible_cpu(cpu) {
		if (!(msm_thermal_info.core_control_mask & BIT(cpu)))
			continue;
		cpus[cpu].user_offline = !!(val & BIT(cpu));
	}

	if (hotplug_task)
		complete(&hotplug_notify_complete);
	else
		pr_err("%s: Hotplug task is not initialized\n", KBUILD_MODNAME);
done_cc:
	mutex_unlock(&core_control_mutex);
	return count;
}

static __refdata struct kobj_attribute cc_enabled_attr =
__ATTR(enabled, 0644, show_cc_enabled, store_cc_enabled);

/* Throttle CPU when reaches a certain tempertature*/
unsigned int temp_threshold = 42;
module_param(temp_threshold, int, 0644);

/* check every 0.5 seconds for the CPU temperature */
unsigned int temp_scan_interval = 500;
module_param(temp_scan_interval, int, 0644);

static struct thermal_info {
	uint32_t cpuinfo_max_freq;
	uint32_t limited_max_freq;
	unsigned int safe_diff;
	bool throttling;
	bool pending_change;
} info = {
	.cpuinfo_max_freq = LONG_MAX,
	.limited_max_freq = LONG_MAX,
	.safe_diff = 5,
	.throttling = false,
	.pending_change = false,
};

enum thermal_freqs {
	FREQ_HELL		= 787200,
	FREQ_VERY_HOT		= 998400,
	FREQ_HOT		= 1190400,
	FREQ_WARM		= 1401600,
static ssize_t show_wakeup_ms(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", wakeup_ms);
}

static ssize_t store_wakeup_ms(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret;
	ret = kstrtouint(buf, 10, &wakeup_ms);

	if (ret) {
		pr_err("%s: Trying to set invalid wakeup timer\n",
				KBUILD_MODNAME);
		return ret;
	}

	if (wakeup_ms > 0) {
		thermal_rtc_setup();
		pr_debug("%s: Timer started for %ums\n", KBUILD_MODNAME,
				wakeup_ms);
	} else {
		ret = android_alarm_cancel(&thermal_rtc);
		if (ret)
			pr_debug("%s: Timer canceled\n", KBUILD_MODNAME);
		else
			pr_debug("%s: No active timer present to cancel\n",
					KBUILD_MODNAME);

	}
	return count;
}

static __refdata struct kobj_attribute timer_attr =
__ATTR(wakeup_ms, 0644, show_wakeup_ms, store_wakeup_ms);

static __refdata struct attribute *tt_attrs[] = {
	&timer_attr.attr,
	NULL,
};

enum threshold_levels {
	LEVEL_HELL		= 1 << 4,
	LEVEL_VERY_HOT		= 1 << 3,
	LEVEL_HOT		= 1 << 2,
};

struct qpnp_vadc_chip *vadc_dev;

enum qpnp_vadc_channels adc_chan;

static struct delayed_work check_temp_work;

unsigned short get_threshold(void)
{
	return temp_threshold;
}

static int msm_thermal_cpufreq_callback(struct notifier_block *nfb,
		unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;

	if (event != CPUFREQ_ADJUST && !info.pending_change)
		return 0;

	cpufreq_verify_within_limits(policy, policy->cpuinfo.min_freq,
		info.limited_max_freq);

	return 0;
}

static struct notifier_block msm_thermal_cpufreq_notifier = {
	.notifier_call = msm_thermal_cpufreq_callback,
};

static void limit_cpu_freqs(uint32_t max_freq)
{
	unsigned int cpu;

	if (info.limited_max_freq == max_freq)
		return;

	info.limited_max_freq = max_freq;

	info.pending_change = true;

	get_online_cpus();
	for_each_online_cpu(cpu)
	{
		cpufreq_update_policy(cpu);
		pr_info("%s: Setting cpu%d max frequency to %d\n",
				KBUILD_MODNAME, cpu, info.limited_max_freq);
	}
	put_online_cpus();

	info.pending_change = false;
}

static void check_temp(struct work_struct *work)
{
	struct qpnp_vadc_result result;
	uint32_t freq = 0;
	int64_t temp;

	qpnp_vadc_read(vadc_dev, adc_chan, &result);
	temp = result.physical;

	if (info.throttling)
	{
		if (temp < (temp_threshold - info.safe_diff))
		{
			limit_cpu_freqs(info.cpuinfo_max_freq);
			info.throttling = false;
			goto reschedule;
		}
	}

	if (temp >= temp_threshold + LEVEL_HELL)
		freq = FREQ_HELL;
	else if (temp >= temp_threshold + LEVEL_VERY_HOT)
		freq = FREQ_VERY_HOT;
	else if (temp >= temp_threshold + LEVEL_HOT)
		freq = FREQ_HOT;
	else if (temp > temp_threshold)
		freq = FREQ_WARM;

	if (freq)
	{
		limit_cpu_freqs(freq);

		if (!info.throttling)
			info.throttling = true;
	}

reschedule:
	schedule_delayed_work_on(0, &check_temp_work, msecs_to_jiffies(temp_scan_interval));
}

static int msm_thermal_dev_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	int ret;

	vadc_dev = qpnp_get_vadc(&pdev->dev, "thermal");

	ret = of_property_read_u32(np, "qcom,adc-channel", &adc_chan);
	if (ret) {
		return ret;
	}

	cpufreq_register_notifier(&msm_thermal_cpufreq_notifier,
			CPUFREQ_POLICY_NOTIFIER);

	INIT_DELAYED_WORK(&check_temp_work, check_temp);
        schedule_delayed_work_on(0, &check_temp_work, 5);

	return ret;
}

static int msm_thermal_dev_remove(struct platform_device *pdev)
{
	cpufreq_unregister_notifier(&msm_thermal_cpufreq_notifier,
                        CPUFREQ_POLICY_NOTIFIER);
	return 0;
}

static struct of_device_id msm_thermal_match_table[] = {
	{.compatible = "qcom,msm-thermal-simple"},
	{},
};

static struct platform_driver msm_thermal_device_driver = {
	.probe = msm_thermal_dev_probe,
	.remove = msm_thermal_dev_remove,
	.driver = {
		.name = "msm-thermal-simple",
		.owner = THIS_MODULE,
		.of_match_table = msm_thermal_match_table,
	},
};

int __init  msm_thermal_device_init(void)
{
	return platform_driver_register(&msm_thermal_device_driver);
}

void __exit msm_thermal_device_exit(void)
{
	platform_driver_unregister(&msm_thermal_device_driver);
	if (num_possible_cpus() > 1)
		msm_thermal_add_cc_nodes();
	msm_thermal_add_psm_nodes();
	msm_thermal_add_vdd_rstr_nodes();
	msm_thermal_add_ocr_nodes();
	android_alarm_init(&thermal_rtc, ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP,
			thermal_rtc_callback);
	INIT_WORK(&timer_work, timer_work_fn);
	msm_thermal_add_timer_nodes();

	return 0;
}

late_initcall(msm_thermal_device_init);
module_exit(msm_thermal_device_exit);
