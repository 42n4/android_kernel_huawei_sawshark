/* Copyright (c) 2013-2015 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/i2c.h>
#include <linux/debugfs.h>
#include <linux/gpio.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/power_supply.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/bitops.h>
#include <linux/mutex.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>
#include <linux/pinctrl/consumer.h>
#include <linux/qpnp/qpnp-adc.h>
#include <linux/kthread.h>
#include <linux/sched.h>

/* Mask/Bit helpers */
#define _mp2661_MASK(BITS, POS) \
    ((unsigned char)(((1 << (BITS)) - 1) << (POS)))
#define mp2661_MASK(LEFT_BIT_POS, RIGHT_BIT_POS) \
        _mp2661_MASK((LEFT_BIT_POS) - (RIGHT_BIT_POS) + 1, \
                (RIGHT_BIT_POS))

/*Input Source Control Register*/
#define INPUT_SOURCE_CTRL_REG                          0x0
#define EN_HIZ_MASK                                    mp2661_MASK(7, 7)
#define EN_HIZ_MASK_SHIFT                              7
#define INPUT_SOURCE_VOLTAGE_LIMIT_MASK                mp2661_MASK(6, 3)
#define INPUT_SOURCE_VOLTAGE_LIMIT_MASK_SHIFT          3
#define INPUT_SOURCE_CURRENT_LIMIT_MASK                mp2661_MASK(2, 0)
#define INPUT_SOURCE_CURRENT_LIMIT_MASK_SHIFT          0

/*Power-On Configuration register*/
#define POWER_ON_CFG_REG                               0x1
#define CHG_ENABLE_BIT                                 BIT(3)
#define UVLO_THRESHOLD_MASK                            mp2661_MASK(2, 0)
#define UVLO_THRESHOLD_MASK_SHIFT                      0

/* Config/Control register */
#define CHG_CURRENT_CTRL_REG                           0x2
#define BATT_CHARGING_CURRENT_MASK                     mp2661_MASK(4, 0)
#define BATT_CHARGING_CURRENT_MASK_SHIFT               0

/*Discharge/termination  current limit register*/
#define DISCHG_TERM_CURRENT_REG                        0x3
#define DISCHG_CURRENT_MASK                            mp2661_MASK(6, 3)
#define DISCHG_CURRENT_MASK_SHIT                       3
#define TRCIKE_PCB_OTP_DISABLE_MASK                    mp2661_MASK(2, 2)
#define TRCIKE_PCB_OTP_DISABLE_MASK_SHIFT              2
#define TRCIKE_CHARGING_CURRENT_MASK                   mp2661_MASK(1, 0)
#define TRCIKE_CHARGING_CURRENT_MASK_SHIFT             0

/*Charge Voltage Control Register*/
#define CHG_VOLTAGE_CTRL_REG                           0x4
#define FULL_CHG_VOLTAGE_MASK                          mp2661_MASK(7, 2)
#define FULL_CHG_VOLTAGE_MASK_SHIF                     2
#define TRICKLE_CHARGE_THESHOLD_MASK                   mp2661_MASK(1, 1)
#define TRICKLE_CHARGE_THESHOLD_MASK_SHIFT             1
#define BATTERY_RECHARGE_THRESHOLD_MASK                mp2661_MASK(0, 0)
#define BATTERY_RECHARGE_THRESHOLD_MASK_SHIFT          0

/*Charge Termination/Timer Control Register */
#define CHG_TERMINATION_TIMER_CTRL_REG                 0x5
#define TERMINATION_EN_MASK                            mp2661_MASK(6, 6)
#define TERMINATION_EN_MASK_SHIFT                      6
#define I2C_WATCHDOG_TIMER_LIMIT_MASK                  mp2661_MASK(5, 4)
#define I2C_WATCHDOG_TIMER_LIMIT_MASK_SHIFT            4
#define SAFETY_TIMER_MASK                              mp2661_MASK(3, 3)
#define SAFETY_TIMER_MASK_SHIFT                        3
#define CC_CHG_TIMER_MASK                              mp2661_MASK(2, 1)
#define CC_CHG_TIMER_MASK_SHIFT                        1

/*Miscellaneous Operation Control Register*/
#define MISCELLANEOUS_OPER_CTRL_REG                    0x6
#define THERMAL_REGULATION_THRESHOLD_MASK              mp2661_MASK(1, 0)
#define THERMAL_REGULATION_THRESHOLD_MASK_SHIFT        0
#define NTC_EN_MASK                                    mp2661_MASK(3, 3)
#define NTC_EN_MASK_SHIFT                              3
#define BAT_FET_DIS_MASK                               mp2661_MASK(5, 5)
#define BAT_FET_DIS_MASK_SHIFT                         5
#define TMR2X_EN_MASK                                  mp2661_MASK(6, 6)
#define TMR2X_EN_MASK_SHIFT                            6

/* System Status Register */
#define SYSTEM_STATUS_REG                              0x07
#define CHG_STAT_MASK                                  mp2661_MASK(4, 3)
#define CHG_STAT_SHIFT                                 3
#define CHAG_IN_VALID_IRQ                              BIT(1)

/* Fault Register */
#define FAULT_REG                                     0x08

enum {
    CHG_STAT_NOT_CHARGING =0,
    CHG_STAT_TRICKE_CHARGE,
    CHG_STAT_CONSTANT_CHARGE,
    CHG_STAT_CHARGE_DONE,
};

enum {
    BAT_TEMP_STATUS_COLD = 0,
    BAT_TEMP_STATUS_NORMAL_STATE1,
    BAT_TEMP_STATUS_NORMAL_STATE2,
    BAT_TEMP_STATUS_NORMAL_STATE3,
    BAT_TEMP_STATUS_NORMAL_STATE4,
    BAT_TEMP_STATUS_HOT,
    BAT_TEMP_STATUS_MAX,
};

static char *pm_batt_supplied_to[] = {
    "bms",
};

struct mp2661_chg {
    struct i2c_client        *client;
    struct device            *dev;
    struct mutex            read_write_lock;

    bool                usb_present;
    int                 charging_status;
    int                fake_battery_soc;
    struct dentry            *debug_root;

    /* psy */
    struct power_supply        *usb_psy;
    struct power_supply        batt_psy;
    struct power_supply        *bms_psy;
    const char            *bms_psy_name;

    struct work_struct        process_interrupt_work;

    /* adc_tm parameters */
    struct qpnp_vadc_chip    *vadc_dev;
    struct qpnp_adc_tm_chip    *adc_tm_dev;
#if 0
    struct qpnp_adc_tm_btm_param    adc_param;
#endif

    bool               using_pmic_therm;
    /* batt temp states decidegc */
    int                cold_batt_decidegc;
    int                normal_state1_batt_decidegc;
    int                normal_state2_batt_decidegc;
    int                normal_state3_batt_decidegc;
    int                hot_batt_decidegc;

    /* charge parameters */
    int                batt_full_mv;
    int                batt_full_terminate_ma;
    int                usb_input_ma;
    int                usb_input_regulation_mv;
    int                batt_charging_ma_max;
    int                batt_temp_status;
    int                batt_chaging_ma_mitigation[BAT_TEMP_STATUS_MAX];
    int                batt_trickle_charging_ma;
    int                batt_trickle_to_cc_theshold_mv;
    int                batt_uvlo_theshold_mv;
    int                batt_auto_recharge_delta_mv;
    int                batt_discharging_ma;
    int                thermal_regulation_threshold;
    int                batt_cc_chg_timer;

    /* monitor temp task */
    struct task_struct       *monitor_temp_task;
    struct semaphore         monitor_temp_sem;
    struct timespec          resume_time;
    struct timespec          last_monitor_time;
    int                      last_temp;

    /* ap mask rx int gpio */
    bool                     ap_mask_rx_int_gpio;

    /* step charging */
    int                step_charging_batt_full_mv;
    int                step_charging_current_ma;
    int                step_charging_delta_voltage_mv;
    int                batt_full_now_mv;
    int                batt_charging_current_now_ma;
};

struct mp2661_chg  *global_mp2661 = NULL;

#define RETRY_COUNT 5
int retry_sleep_ms[RETRY_COUNT] = {
    10, 20, 30, 40, 50
};

static int __mp2661_read(struct mp2661_chg *chip, int reg,
                u8 *val)
{
    s32 ret;
    int retry_count = 0;

retry:
    ret = i2c_smbus_read_byte_data(chip->client, reg);
    if (ret < 0 && retry_count < RETRY_COUNT)
    {
        /* sleep for few ms before retrying */
        msleep(retry_sleep_ms[retry_count++]);
        goto retry;
    }
    if (ret < 0)
    {
        pr_err("i2c read fail: can't read from %02x: %d\n", reg, ret);
        return ret;
    }
    else
    {
        *val = ret;
    }

    return 0;
}


static int __mp2661_write(struct mp2661_chg *chip, int reg,
                        u8 val)
{
    s32 ret;
    int retry_count = 0;

retry:
    ret = i2c_smbus_write_byte_data(chip->client, reg, val);
    if (ret < 0 && retry_count < RETRY_COUNT)
    {
        /* sleep for few ms before retrying */
        msleep(retry_sleep_ms[retry_count++]);
        goto retry;
    }
    if (ret < 0)
    {
        pr_err("i2c write fail: can't write %02x to %02x: %d\n",
            val, reg, ret);
        return ret;
    }
    pr_debug("Writing 0x%02x=0x%02x\n", reg, val);
    return 0;
}

static int mp2661_read(struct mp2661_chg *chip, int reg,
                u8 *val)
{
    int rc;

    mutex_lock(&chip->read_write_lock);
    pm_stay_awake(chip->dev);
    rc = __mp2661_read(chip, reg, val);
    pm_relax(chip->dev);
    mutex_unlock(&chip->read_write_lock);

    return rc;
}

static int mp2661_masked_write(struct mp2661_chg *chip, int reg,
                        u8 mask, u8 val)
{
    s32 rc;
    u8 temp;

    mutex_lock(&chip->read_write_lock);
    rc = __mp2661_read(chip, reg, &temp);
    if (rc < 0)
    {
        pr_err("read failed: reg=%03X, rc=%d\n", reg, rc);
        goto out;
    }

    temp &= ~mask;
    temp |= val & mask;
    rc = __mp2661_write(chip, reg, temp);
    if (rc < 0)
    {
        pr_err("write failed: reg=%03X, rc=%d\n", reg, rc);
    }
out:
    mutex_unlock(&chip->read_write_lock);
    return rc;
}


static enum power_supply_property mp2661_battery_properties[] = {
    POWER_SUPPLY_PROP_STATUS,
    POWER_SUPPLY_PROP_PRESENT,
    POWER_SUPPLY_PROP_CHARGING_ENABLED,
    POWER_SUPPLY_PROP_CHARGE_TYPE,
    POWER_SUPPLY_PROP_CAPACITY,
    POWER_SUPPLY_PROP_HEALTH,
    POWER_SUPPLY_PROP_TECHNOLOGY,
    POWER_SUPPLY_PROP_TEMP,
    POWER_SUPPLY_PROP_TEMP_AMBIENT,
    POWER_SUPPLY_PROP_VOLTAGE_NOW,
    POWER_SUPPLY_PROP_CURRENT_NOW,
    POWER_SUPPLY_PROP_USB_INPUT_CURRENT,
    POWER_SUPPLY_PROP_BATTERY_ID,
};

static int mp2661_get_prop_batt_status(struct mp2661_chg *chip)
{
    int rc;
    u8 reg;
    int status = POWER_SUPPLY_STATUS_DISCHARGING;
    u8 chgr_sts = 0;

    rc = mp2661_read(chip, SYSTEM_STATUS_REG, &reg);
    if (rc < 0)
    {
        pr_err("Unable to read system status reg rc = %d\n", rc);
        return POWER_SUPPLY_STATUS_UNKNOWN;
    }
    chgr_sts = (reg & CHG_STAT_MASK) >> CHG_STAT_SHIFT;
    pr_debug("system status reg = %x\n", chgr_sts);

    if(CHG_STAT_NOT_CHARGING == chgr_sts)
    {
        status = POWER_SUPPLY_STATUS_DISCHARGING;
    }
    else if(CHG_STAT_CHARGE_DONE == chgr_sts)
    {
        status = POWER_SUPPLY_STATUS_FULL;
    }
    else
    {
        status = POWER_SUPPLY_STATUS_CHARGING;
    }

    return status;
}

#define BATT_ID_VOLTAGE_REFERENCE_UV            1800000
#define BATT_ID_VOLTAGE_REFERENCE_DELTA_UV        50000
static int mp2661_get_prop_batt_present(struct mp2661_chg *chip)
{
    int rc = 0;
    struct qpnp_vadc_result results;

    rc = qpnp_vadc_read(chip->vadc_dev, LR_MUX2_BAT_ID, &results);
    if (rc)
    {
        pr_debug("Unable to read batt_id rc=%d\n", rc);
        return 0;
    }

    if((results.physical > BATT_ID_VOLTAGE_REFERENCE_UV - BATT_ID_VOLTAGE_REFERENCE_DELTA_UV )
        && (results.physical < BATT_ID_VOLTAGE_REFERENCE_UV + BATT_ID_VOLTAGE_REFERENCE_DELTA_UV))
    {
        return 0;
    }

    return 1;
}

enum {
    POWER_SUPPLY_BATTERY_ID_UNKNOWN = 0,
    POWER_SUPPLY_BATTERY_ID_GUANGYU,
    POWER_SUPPLY_BATTERY_ID_DESAY,
};

#define BATT_ID_VOL_MIN_UV                    0
#define BATT_ID_VOL_AVG_UV                    548038
#define BATT_ID_VOL_MAX_UV                    1600000
static int mp2661_get_prop_batt_id(struct mp2661_chg *chip)
{
    int rc = 0;
    struct qpnp_vadc_result results;
    union power_supply_propval ret = {0, };

    rc = qpnp_vadc_read(chip->vadc_dev, LR_MUX2_BAT_ID, &results);
    if (rc)
    {
        pr_debug("Unable to read batt_id rc=%d\n", rc);
        return POWER_SUPPLY_BATTERY_ID_UNKNOWN;
    }

    if((results.physical > BATT_ID_VOL_MIN_UV) && (results.physical <= BATT_ID_VOL_AVG_UV))
    {
        ret.intval = POWER_SUPPLY_BATTERY_ID_GUANGYU;
    }
    else if((results.physical > BATT_ID_VOL_AVG_UV) && (results.physical <= BATT_ID_VOL_MAX_UV))
    {
        ret.intval = POWER_SUPPLY_BATTERY_ID_DESAY;
    }
    else
    {
        ret.intval = POWER_SUPPLY_BATTERY_ID_UNKNOWN;
    }

    return ret.intval;
}

static int mp2661_get_prop_charge_type(struct mp2661_chg *chip)
{
    return POWER_SUPPLY_CHARGE_TYPE_FAST;
}

#define DEFAULT_BATT_CAPACITY    50
static int mp2661_get_prop_batt_capacity(struct mp2661_chg *chip)
{
    union power_supply_propval ret = {0, };

    if (chip->fake_battery_soc >= 0)
    {
        return chip->fake_battery_soc;
    }

    if (!chip->bms_psy && chip->bms_psy_name)
    {
        pr_info("get bms power supply\n");
        chip->bms_psy = power_supply_get_by_name((char *)chip->bms_psy_name);
    }

    if (chip->bms_psy)
    {
        chip->bms_psy->get_property(chip->bms_psy,
                POWER_SUPPLY_PROP_CAPACITY, &ret);
        return ret.intval;
    }

    return DEFAULT_BATT_CAPACITY;
}

static int mp2661_get_prop_batt_health(struct mp2661_chg *chip)
{

    union power_supply_propval ret = {0, };

    if (BAT_TEMP_STATUS_HOT == chip->batt_temp_status)
    {
        ret.intval = POWER_SUPPLY_HEALTH_OVERHEAT;
    }
    else if (BAT_TEMP_STATUS_COLD == chip->batt_temp_status)
    {
        ret.intval = POWER_SUPPLY_HEALTH_COLD;
    }
    else
    {
        ret.intval = POWER_SUPPLY_HEALTH_GOOD;
    }

    return ret.intval;
}

#define DEFAULT_TEMP 250
static int mp2661_get_prop_batt_temp(struct mp2661_chg *chip)
{

    int rc = 0;
    struct qpnp_vadc_result results;

    rc = qpnp_vadc_read(chip->vadc_dev, P_MUX2_1_1, &results);
    if (rc)
    {
        pr_err("Unable to read batt temperature rc=%d\n", rc);
        return DEFAULT_TEMP;
    }
    pr_debug("get_batt_temp %d, %lld\n",
        results.adc_code, results.physical);

    return (int)results.physical;
}

static int mp2661_get_prop_ambient_temp(struct mp2661_chg *chip)
{
    int rc = 0;
    struct qpnp_vadc_result results;

    rc = qpnp_vadc_read(chip->vadc_dev, P_MUX3_1_1, &results);
    if (rc)
    {
        pr_err("Unable to read ambient temperature rc=%d\n", rc);
        return DEFAULT_TEMP;
    }
    pr_debug("get_ambient_temp %d, %lld\n",
        results.adc_code, results.physical);

    return (int)results.physical;
}

#define DEFAULT_BATT_VOLTAGE    2800000
static int mp2661_get_prop_battery_voltage_now(struct mp2661_chg *chip)
{
    union power_supply_propval ret = {0, };

    if (chip->fake_battery_soc >= 0)
    {
        return chip->fake_battery_soc;
    }

    if (!chip->bms_psy && chip->bms_psy_name)
    {
        pr_info("get bms power supply\n");
        chip->bms_psy = power_supply_get_by_name((char *)chip->bms_psy_name);
    }

    if (chip->bms_psy)
    {
        chip->bms_psy->get_property(chip->bms_psy,
                POWER_SUPPLY_PROP_VOLTAGE_NOW, &ret);
        return ret.intval;
    }

    return DEFAULT_BATT_VOLTAGE;
}

static int mp2661_get_prop_current_now(struct mp2661_chg *chip)
{
    union power_supply_propval ret = {0,};

    if (!chip->bms_psy && chip->bms_psy_name)
    {
        pr_info("get bms power supply\n");
        chip->bms_psy = power_supply_get_by_name((char *)chip->bms_psy_name);
    }

    if (chip->bms_psy)
    {
        chip->bms_psy->get_property(chip->bms_psy,
              POWER_SUPPLY_PROP_CURRENT_NOW, &ret);
        return ret.intval;
    }
    else
    {
        pr_debug("No BMS supply registered return 0\n");
    }

    return 0;
}

#define MP2661_FULL_VOLTAGE_STEP_MV        15
#define MP2661_FULL_VOLTAGE_MIN_MV        3600
#define MP2661_FULL_VOLTAGE_MAX_MV        4545
static int mp2661_set_batt_full_voltage(struct mp2661_chg *chip,
                            int voltage)
{
    int rc,i;

    if ((voltage < MP2661_FULL_VOLTAGE_MIN_MV) ||
        (voltage > MP2661_FULL_VOLTAGE_MAX_MV))
    {
        pr_err( "bad charge full voltage %d asked to set\n",
                            voltage);
        return -EINVAL;
    }

    i = (voltage - MP2661_FULL_VOLTAGE_MIN_MV) / MP2661_FULL_VOLTAGE_STEP_MV;
    i = i << FULL_CHG_VOLTAGE_MASK_SHIF;
    rc = mp2661_masked_write(chip, CHG_VOLTAGE_CTRL_REG,
            FULL_CHG_VOLTAGE_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set batt full voltage to %dmv rc = %d\n", voltage, rc);
    }
    else
    {
        chip->batt_full_now_mv = voltage;
    }
    return rc;
}

static int mp2661_set_batt_full_terminate_current(struct mp2661_chg *chip,
                            int current_limit)
{
    return 0;
}

#define MP2661_USB_INPUT_CURRENT_MIN_MA        85
#define MP2661_USB_INPUT_CURRENT_MAX_MA        455
static int usb_input_current_limit[] = {
    85, 130, 175, 220, 265, 310, 355, 455,
};
static int mp2661_set_usb_input_current(struct mp2661_chg *chip,
                            int current_limit)
{
    int rc, i;

    if ((current_limit < MP2661_USB_INPUT_CURRENT_MIN_MA) ||
        (current_limit > MP2661_USB_INPUT_CURRENT_MAX_MA))
    {
        pr_err( "bad usb input current mA=%d asked to set\n",
                            current_limit);
        return -EINVAL;
    }

    for (i = ARRAY_SIZE(usb_input_current_limit) - 1; i >= 0; i--)
    {
        if (usb_input_current_limit[i] <= current_limit)
            break;
    }

    if (i < 0)
    {
        pr_err("Invalid current setting %dmA\n",
                        current_limit);
        i = 0;
    }

    i = i << INPUT_SOURCE_CURRENT_LIMIT_MASK_SHIFT;
    rc = mp2661_masked_write(chip, INPUT_SOURCE_CTRL_REG,
            INPUT_SOURCE_CURRENT_LIMIT_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set usb input current to %dma rc = %d\n", current_limit, rc);
    }
    return rc;
}

void mp2661_global_set_usb_input_current_default(void)
{
    int rc;

    if(!global_mp2661)
    {
        pr_err("mp2661 chip can not register\n");
        return;
    }

    rc = mp2661_set_usb_input_current(global_mp2661, global_mp2661->usb_input_ma);
    if(rc)
    {
        pr_err("Couldn't set usb input current rc = %d\n", rc);
    }
}

static int mp2661_get_usb_input_current(struct mp2661_chg *chip)
{
    union power_supply_propval ret = {0,};

    int rc;
    u8 reg;

    rc = mp2661_read(chip, INPUT_SOURCE_CTRL_REG, &reg);
    if (rc < 0)
    {
        pr_err("Couldn't read INPUT_SOURCE_CTRL_REG rc = %d\n", rc);
        return rc;
    }

    reg = (reg >> INPUT_SOURCE_CURRENT_LIMIT_MASK_SHIFT) & INPUT_SOURCE_CURRENT_LIMIT_MASK;
    ret.intval =  usb_input_current_limit[reg];

    return ret.intval;
}

#define MP2661_USB_INPUT_VOLTAGE_STEP_MV    80
#define MP2661_USB_INPUT_VOLTAGE_MAX_MV        5080
#define MP2661_USB_INPUT_VOLTAGE_MIN_MV        3880
static int mp2661_set_usb_input_voltage_regulation(struct mp2661_chg *chip,
                            int voltage)
{
    int rc, i;

    if ((voltage < MP2661_USB_INPUT_VOLTAGE_MIN_MV) ||
        (voltage > MP2661_USB_INPUT_VOLTAGE_MAX_MV))
    {
        pr_err( "bad input current mA=%d asked to set\n",
                                voltage);
        return -EINVAL;
    }

    i = (voltage - MP2661_USB_INPUT_VOLTAGE_MIN_MV) / MP2661_USB_INPUT_VOLTAGE_STEP_MV;
    i = i << INPUT_SOURCE_VOLTAGE_LIMIT_MASK_SHIFT;
    rc = mp2661_masked_write(chip, INPUT_SOURCE_CTRL_REG,
            INPUT_SOURCE_VOLTAGE_LIMIT_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set usb input voltage regulation to %dmv rc = %d\n", voltage, rc);
    }
    return rc;
}

#define MP2661_BATT_CHARGING_STEP_MA      17
#define MP2661_BATT_CHARGING_MIN_MA        8
#define MP2661_BATT_CHARGING_MAX_MA        535
static int mp2661_set_batt_charging_current(struct mp2661_chg *chip,
                            int current_ma)
{
    int rc, i;

    if ((current_ma < MP2661_BATT_CHARGING_MIN_MA) ||
        (current_ma > MP2661_BATT_CHARGING_MAX_MA))
    {
        pr_err( "bad bat charging current mA=%d asked to set\n",
                            current_ma);
        return -EINVAL;
    }

    i = (current_ma - MP2661_BATT_CHARGING_MIN_MA) / MP2661_BATT_CHARGING_STEP_MA;
    i = i << BATT_CHARGING_CURRENT_MASK_SHIFT;
    rc = mp2661_masked_write(chip, CHG_CURRENT_CTRL_REG,
            BATT_CHARGING_CURRENT_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set batt charging current to %dma rc = %d\n", current_ma, rc);
    }
    else
    {
        chip->batt_charging_current_now_ma = current_ma;
    }
    return rc;
}

extern int mp2661_global_get_batt_charging_current(void)
{
    union power_supply_propval ret = {0,};
    int rc;
    u8 reg;

    rc = mp2661_read(global_mp2661, CHG_CURRENT_CTRL_REG, &reg);
    if (rc < 0)
    {
        pr_err("Couldn't read CHG_CURRENT_CTRL_REG rc = %d\n", rc);
        return rc;
    }

    reg = (reg >> BATT_CHARGING_CURRENT_MASK_SHIFT) & BATT_CHARGING_CURRENT_MASK;
    reg = reg >> BATT_CHARGING_CURRENT_MASK_SHIFT;
    ret.intval = reg * MP2661_BATT_CHARGING_STEP_MA + MP2661_BATT_CHARGING_MIN_MA;

    return ret.intval;
}

static void mp2661_set_appropriate_batt_charging_current(
                struct mp2661_chg *chip)
{
    int rc;
    int current_max = chip->batt_charging_ma_max;
    int index = chip->batt_temp_status;

    current_max =
                min(current_max, chip->batt_chaging_ma_mitigation[index]);
    pr_info("setting %dma", current_max);

    rc = mp2661_set_batt_charging_current(chip, current_max);
    if (rc)
    {
        pr_err("Couldn't set batt appopriate batt charging current rc = %d\n", rc);
    }
}

static int mp2661_set_pcb_otp_disable(struct mp2661_chg *chip,
                            bool disable)
{
    int rc, i;

    i = disable << TRCIKE_PCB_OTP_DISABLE_MASK_SHIFT;
    rc = mp2661_masked_write(chip, DISCHG_TERM_CURRENT_REG,
            TRCIKE_PCB_OTP_DISABLE_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set pcb otp disable to %d rc = %d\n", disable, rc);
    }
    return rc;
}

#define MP2661_TRICKE_CHARGING_STEP_MA        7
#define MP2661_TRICKLE_CHARGING_MIN_MA        6
#define MP2661_TRICKLE_CHARGING_MAX_MA        27
static int mp2661_set_batt_trickle_charging_current(struct mp2661_chg *chip,
                            int current_limit)
{
    int rc,i;

    if ((current_limit < MP2661_TRICKLE_CHARGING_MIN_MA) ||
        (current_limit > MP2661_TRICKLE_CHARGING_MAX_MA))
    {
        pr_err( "bad trickle charging current limit mA=%d asked to set\n",
                            current_limit);
        return -EINVAL;
    }

    i = (current_limit - MP2661_TRICKLE_CHARGING_MIN_MA) / MP2661_TRICKE_CHARGING_STEP_MA;
    i = i << TRCIKE_CHARGING_CURRENT_MASK_SHIFT;
    rc = mp2661_masked_write(chip, DISCHG_TERM_CURRENT_REG,
            TRCIKE_CHARGING_CURRENT_MASK, i);
    if (rc < 0)
    {
        pr_err("cannotset batt trickle chargint current to %dma rc = %d\n", current_limit, rc);
    }
    return rc;
}

#define MP2661_BATT_TRICKLE_TO_CC_THRESHOLD_MIN_MV        2800
#define MP2661_BATT_TRICKLE_TO_CC_THRESHOLD_MAX_MV        3000
static int mp2661_set_batt_trickle_to_cc_threshold(struct mp2661_chg *chip,
                            int voltage)
{
    int rc,i;

    if(MP2661_BATT_TRICKLE_TO_CC_THRESHOLD_MIN_MV == voltage)
    {
        i = 0;
    }
    else if(MP2661_BATT_TRICKLE_TO_CC_THRESHOLD_MAX_MV == voltage)
    {
        i = 1;
    }
    else
    {
        pr_err( "bad batt trickle to cc mv=%d asked to set\n",
                                voltage);
        return -EINVAL;
    }

    i = i << TRICKLE_CHARGE_THESHOLD_MASK_SHIFT;
    rc = mp2661_masked_write(chip, CHG_VOLTAGE_CTRL_REG,
            TRICKLE_CHARGE_THESHOLD_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set batt trickle to cc threshold to %dmv rc = %d\n", voltage, rc);
    }
    return rc;
}

#define MP2661_UVLO_THRESHOLD_STEP_MV        100
#define MP2661_UVLO_THRESHOLD_MIN_MV        2400
#define MP2661_UVLO_THRESHOLD_MAX_MV        3100
static int mp2661_set_batt_uvlo_threshold(struct mp2661_chg *chip,
                            int voltage)
{
    int rc, i;

    if ((voltage < MP2661_UVLO_THRESHOLD_MIN_MV) ||
        (voltage > MP2661_UVLO_THRESHOLD_MAX_MV))
    {
        pr_err( "bad batt uvlo threshold mv=%d asked to set\n",
                            voltage);
        return -EINVAL;
    }

    i = (voltage - MP2661_UVLO_THRESHOLD_MIN_MV) / MP2661_UVLO_THRESHOLD_STEP_MV;
    i = i << UVLO_THRESHOLD_MASK_SHIFT;
    rc = mp2661_masked_write(chip, POWER_ON_CFG_REG,
            UVLO_THRESHOLD_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set batt uvlo threshold to %dmv rc = %d\n", voltage, rc);
    }
    return rc;
}

#define MP2661_AUTO_RECHARGE_BELOW_FULL_MIN_MV        150
#define MP2661_AUTO_RECHARGE_BELOW_FULL_MAX_MV        300
static int mp2661_set_batt_auto_recharge(struct mp2661_chg *chip,
                            int voltage_below_full)
{
    int rc,i;

    if(MP2661_AUTO_RECHARGE_BELOW_FULL_MIN_MV == voltage_below_full)
    {
        i = 0;
    }
    else if(MP2661_AUTO_RECHARGE_BELOW_FULL_MAX_MV == voltage_below_full)
    {
        i = 1;
    }
    else
    {
        pr_err( "bad auto recharge current below full mv=%d asked to set\n",
                                    voltage_below_full);
        return -EINVAL;
    }

    i = i << BATTERY_RECHARGE_THRESHOLD_MASK_SHIFT;
    rc = mp2661_masked_write(chip, CHG_VOLTAGE_CTRL_REG,
                BATTERY_RECHARGE_THRESHOLD_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set batt auto recharge below full to %dmv rc = %d\n", voltage_below_full, rc);
    }
    return rc;
}

#define MP2661_BATT_DISCHARGE_CURRENT_STEP_MA  200
#define MP2661_BATT_DISCHARGE_MIN_MA        200
#define MP2661_BATT_DISCHARGE_MAX_MA        3200
static int mp2661_set_batt_discharging_current(struct mp2661_chg *chip,
                            int current_limit)
{
    int rc, i;

    if ((current_limit < MP2661_BATT_DISCHARGE_MIN_MA) ||
        (current_limit > MP2661_BATT_DISCHARGE_MAX_MA))
    {
        pr_err( "bad batt dischargge current limit ma=%d asked to set\n",
                            current_limit);
        return -EINVAL;
    }

    i = (current_limit - MP2661_BATT_DISCHARGE_MIN_MA) / MP2661_BATT_DISCHARGE_CURRENT_STEP_MA;
    i = i << DISCHG_CURRENT_MASK_SHIT;
    rc = mp2661_masked_write(chip, DISCHG_TERM_CURRENT_REG,
            DISCHG_CURRENT_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set batt discharging current to %dma rc = %d\n", current_limit, rc);
    }
    return rc;
}

#define MP2661_THERMAL_TEMP_STEP       20
#define MP2661_THERMAL_TEMP_MAX        120
#define MP2661_THERMAL_TEMP_MIN        60
static int mp2661_set_thermal_regulation_threshold(struct mp2661_chg *chip,
                            int temp)
{
    int rc, i;

    if ((temp < MP2661_THERMAL_TEMP_MIN) ||
        (temp > MP2661_THERMAL_TEMP_MAX))
    {
        pr_err("bad input temp = %d asked to set\n",
                                temp);
        return -EINVAL;
    }

    i = (temp - MP2661_THERMAL_TEMP_MIN) / MP2661_THERMAL_TEMP_STEP;
    i = i << THERMAL_REGULATION_THRESHOLD_MASK_SHIFT;
    rc = mp2661_masked_write(chip, MISCELLANEOUS_OPER_CTRL_REG,
            THERMAL_REGULATION_THRESHOLD_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set thermal regulation threshold to %d, rc = %d\n", temp, rc);
    }
    return rc;
}

static int mp2661_en_bf_enable(struct mp2661_chg *chip,
                            bool enable)
{
    int rc,i;

    i = enable << TERMINATION_EN_MASK_SHIFT;
    rc = mp2661_masked_write(chip, CHG_TERMINATION_TIMER_CTRL_REG,
            TERMINATION_EN_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set bf enable to %d rc = %d\n", enable, rc);
    }
    return rc;
}

static int mp2661_set_ldo_fet_disconnect(struct mp2661_chg *chip,
                            bool disconnect)
{
    int rc,i;

    i = disconnect << EN_HIZ_MASK_SHIFT;
    rc = mp2661_masked_write(chip, INPUT_SOURCE_CTRL_REG,
            EN_HIZ_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set ldo fet disconnect to %d rc = %d\n", disconnect, rc);
    }
    return rc;
}

static int mp2661_set_batt_fet_disconnect(struct mp2661_chg *chip,
                            bool disconnect)
{
    int rc,i;

    i = disconnect << BAT_FET_DIS_MASK_SHIFT;
    rc = mp2661_masked_write(chip, MISCELLANEOUS_OPER_CTRL_REG,
            BAT_FET_DIS_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set batt fet disconnect to %d rc = %d\n", disconnect, rc);
    }
    return rc;
}

static int mp2661_tmr2x_enable(struct mp2661_chg *chip,
                            bool enable)
{
    int rc,i;

    i = enable << TMR2X_EN_MASK_SHIFT;
    rc = mp2661_masked_write(chip, MISCELLANEOUS_OPER_CTRL_REG,
                TMR2X_EN_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set tmr2x enable to %d rc = %d\n", enable, rc);
    }
    return rc;
}

static int mp2661_ntc_enable(struct mp2661_chg *chip,
                            bool enable)
{
    int rc,i;

    i = enable << NTC_EN_MASK_SHIFT;
    rc = mp2661_masked_write(chip, MISCELLANEOUS_OPER_CTRL_REG,
                NTC_EN_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set ntc enable to %d rc = %d\n", enable, rc);
    }
    return rc;
}

#define MP2661_I2C_WATCHDOG_TIMER_STEP_SEC    40
#define MP2661_I2C_WATCHDOG_TIMER_MIN_SEC    0
#define MP2661_I2C_WATCHDOG_TIMER_MAX_SEC    160
static int mp2661_set_i2c_watchdog_timer(struct mp2661_chg *chip,
                            int time)
{
    int rc,i;

    if((time > MP2661_I2C_WATCHDOG_TIMER_MAX_SEC) ||
        (time < MP2661_I2C_WATCHDOG_TIMER_MIN_SEC))
    {
        pr_err("bad i2c watchdog timer sec=%d asked to set\n",
                                time);
        return -EINVAL;
    }

    i = time / MP2661_I2C_WATCHDOG_TIMER_STEP_SEC;
    i = i << I2C_WATCHDOG_TIMER_LIMIT_MASK_SHIFT;
    rc = mp2661_masked_write(chip, CHG_TERMINATION_TIMER_CTRL_REG,
            I2C_WATCHDOG_TIMER_LIMIT_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set i2c watchdog timer to %ds rc = %d\n", time, rc);
    }
    return rc;
}

static int mp2661_safety_timer_enable(struct mp2661_chg *chip,
                            bool enable)
{
    int rc,i;

    i = enable << SAFETY_TIMER_MASK_SHIFT;
    rc = mp2661_masked_write(chip, CHG_TERMINATION_TIMER_CTRL_REG,
            SAFETY_TIMER_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set safety timer enable to %d rc = %d\n", enable, rc);
    }
    return rc;
}

static int cc_chg_timer[4] = {
    3, 5, 8, 12
};
static int mp2661_set_cc_chg_timer(struct mp2661_chg *chip,
                            int time_limit)
{
    int rc,i;

    if ((time_limit < cc_chg_timer[0]) ||
        (time_limit > cc_chg_timer[3]))
    {
        pr_err( "bad cc chg timer hours=%d asked to set\n",
                            time_limit);
        return -EINVAL;
    }

    for (i = ARRAY_SIZE(cc_chg_timer) - 1; i >= 0; i--)
    {
        if (cc_chg_timer[i] <= time_limit)
        {
            break;
        }
    }

    if (i < 0)
    {
        pr_err("Invalid cc chg timer, setting default timer to %d hours\n",
                        cc_chg_timer[0]);
        i = 0;
    }

    i = i << CC_CHG_TIMER_MASK_SHIFT;
    rc = mp2661_masked_write(chip, CHG_TERMINATION_TIMER_CTRL_REG,
            CC_CHG_TIMER_MASK, i);
    if (rc < 0)
    {
        pr_err("cannot set cc chg timer rc = %d\n", rc);
    }
    return rc;
}

static int mp2661_set_charging_enable(struct mp2661_chg *chip, bool enable)
{
    int rc;

    rc = mp2661_masked_write(chip, POWER_ON_CFG_REG,
                CHG_ENABLE_BIT, enable ? 0 : CHG_ENABLE_BIT);
    if (rc < 0)
    {
        pr_err("Couldn't set CHG_ENABLE_BIT enable = %d rc = %d\n",    enable, rc);
        return rc;
    }

    return 0;
}

static int mp2661_is_charging_enable(struct mp2661_chg *chip)
{

    int rc;
    u8 reg;

    rc = mp2661_read(chip, POWER_ON_CFG_REG, &reg);
    if (rc < 0)
    {
        pr_err("Couldn't read power-on config reg rc = %d\n", rc);
        return rc;
    }

    return (reg & CHG_ENABLE_BIT) ? 0 : 1;
}

static int mp2661_is_chg_plugged_in(struct mp2661_chg *chip)
{
    int rc;
    u8 reg;

    rc = mp2661_read(chip, SYSTEM_STATUS_REG, &reg);
    if (rc < 0)
    {
        pr_err("Couldn't read system status reg rc = %d\n", rc);
        return POWER_SUPPLY_STATUS_UNKNOWN;
    }

    return (reg & CHAG_IN_VALID_IRQ) ? 1 : 0;
}

static int mp2661_battery_set_property(struct power_supply *psy,
                       enum power_supply_property prop,
                       const union power_supply_propval *val)
{
    int rc = 0, update_psy = 0;
    struct mp2661_chg *chip = container_of(psy,
                struct mp2661_chg, batt_psy);

    switch (prop)
    {
        case POWER_SUPPLY_PROP_CHARGING_ENABLED:
            rc = mp2661_set_charging_enable(chip, val->intval);
            update_psy = 1;
            break;
        case POWER_SUPPLY_PROP_USB_INPUT_CURRENT:
            rc = mp2661_set_usb_input_current(chip, val->intval);
            update_psy = 1;
            break;
        default:
            rc = -EINVAL;
    }

    if (!rc && update_psy)
    {
        power_supply_changed(&chip->batt_psy);
    }
    return rc;
}

static int mp2661_battery_get_property(struct power_supply *psy,
                       enum power_supply_property prop,
                       union power_supply_propval *val)
{
    struct mp2661_chg *chip = container_of(psy,
                struct mp2661_chg, batt_psy);

    switch (prop)
    {
        case POWER_SUPPLY_PROP_STATUS:
            val->intval = mp2661_get_prop_batt_status(chip);
            break;
        case POWER_SUPPLY_PROP_PRESENT:
            val->intval = mp2661_get_prop_batt_present(chip);
            break;
        case POWER_SUPPLY_PROP_CHARGING_ENABLED:
            val->intval = mp2661_is_charging_enable(chip);
            break;
        case POWER_SUPPLY_PROP_CHARGE_TYPE:
            val->intval = mp2661_get_prop_charge_type(chip);
            break;
        case POWER_SUPPLY_PROP_CAPACITY:
            val->intval = mp2661_get_prop_batt_capacity(chip);
            break;
        case POWER_SUPPLY_PROP_HEALTH:
            val->intval = mp2661_get_prop_batt_health(chip);
            break;
        case POWER_SUPPLY_PROP_TECHNOLOGY:
            val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
            break;
        case POWER_SUPPLY_PROP_TEMP:
            val->intval = mp2661_get_prop_batt_temp(chip);
            break;
        case POWER_SUPPLY_PROP_TEMP_AMBIENT:
            val->intval = mp2661_get_prop_ambient_temp(chip);
            break;
        case POWER_SUPPLY_PROP_VOLTAGE_NOW:
            val->intval = mp2661_get_prop_battery_voltage_now(chip);
            break;
        case POWER_SUPPLY_PROP_CURRENT_NOW:
            val->intval = mp2661_get_prop_current_now(chip);
            break;
        case POWER_SUPPLY_PROP_USB_INPUT_CURRENT:
            val->intval = mp2661_get_usb_input_current(chip);
            break;
        case POWER_SUPPLY_PROP_BATTERY_ID:
            val->intval = mp2661_get_prop_batt_id(chip);
            break;
        default:
            return -EINVAL;
    }
    return 0;
}

static void mp2661_external_power_changed(struct power_supply *psy)
{
    struct mp2661_chg *chip = container_of(psy,
            struct mp2661_chg, batt_psy);

    if (!chip->bms_psy && chip->bms_psy_name)
    {
        chip->bms_psy = power_supply_get_by_name((char *)chip->bms_psy_name);
    }
}

static void mp2661_process_interrupt_work(struct work_struct *work)
{
    int status, usb_present;
    struct mp2661_chg *chip = container_of(work, struct mp2661_chg, process_interrupt_work);

    /* check charging status */
    status = mp2661_get_prop_batt_status(chip);
    if(chip->charging_status != status)
    {
        pr_debug("charing status change from %d to %d\n", chip->charging_status, status);
        chip->charging_status = status;
        if(POWER_SUPPLY_STATUS_FULL == status)
        {
            pr_info("battery is full\n");
            if (!chip->bms_psy && chip->bms_psy_name)
            {
                pr_info("get bms power supply\n");
                chip->bms_psy = power_supply_get_by_name((char *)chip->bms_psy_name);
            }

            if(chip->bms_psy)
            {
                power_supply_changed(chip->bms_psy);
            }
            else
            {
                pr_err("bms_psy is NULL\n");
            }
        }
    }

    /* check usb status */
    usb_present = mp2661_is_chg_plugged_in(chip);
    if (chip->usb_present != usb_present)
    {
        chip->usb_present = usb_present;
        pr_info("usb_present = %d\n", chip->usb_present);

        power_supply_set_present(chip->usb_psy, chip->usb_present);
        pr_info("usb psy changed\n");
        power_supply_changed(chip->usb_psy);
    }
 }

static irqreturn_t mp2661_chg_stat_handler(int irq, void *dev_id)
{
    struct mp2661_chg *chip = dev_id;

    schedule_work(&chip->process_interrupt_work);

    return IRQ_HANDLED;
}

static struct of_device_id mp2661_match_table[] = {
    {
        .compatible    = "qcom,mp2661-charger",
    },
    { },
};

static int mp2661_parse_dt(struct mp2661_chg *chip)
{
    int rc;
    struct device_node *node = chip->dev->of_node;

    rc = of_property_read_string(node, "qcom,bms-psy-name",
                            &chip->bms_psy_name);
    if (rc)
    {
        chip->bms_psy_name = NULL;
    }

    chip->using_pmic_therm = of_property_read_bool(node,
                        "qcom,using-pmic-therm");

    rc = of_property_read_u32(node, "qcom,cold-batt-decidegc",
                        &chip->cold_batt_decidegc);
    if (rc)
    {
        chip->cold_batt_decidegc = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,normal-state1-batt-decidegc",
                        &chip->normal_state1_batt_decidegc);
    if (rc < 0)
    {
        chip->cold_batt_decidegc = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,normal-state2-batt-decidegc",
                            &chip->normal_state2_batt_decidegc);
    if (rc < 0)
    {
        chip->normal_state2_batt_decidegc = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,normal-state3-batt-decidegc",
                        &chip->normal_state3_batt_decidegc);
    if (rc < 0)
    {
        chip->normal_state3_batt_decidegc = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,hot-batt-decidegc",
                        &chip->hot_batt_decidegc);
    if (rc < 0)
    {
        chip->hot_batt_decidegc = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,batt-full-mv",
                            &chip->batt_full_mv);
    if (rc < 0)
    {
        chip->batt_full_mv = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,batt-full-terminate-ma",
                            &chip->batt_full_terminate_ma);
    if (rc < 0)
    {
        chip->batt_full_terminate_ma = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,usb-input-ma",
                            &chip->usb_input_ma);
    if (rc < 0)
    {
        chip->usb_input_ma = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,usb-input-regulation-mv",
                            &chip->usb_input_regulation_mv);
    if (rc < 0)
    {
        chip->usb_input_regulation_mv = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,batt-charging-ma-max",
                            &chip->batt_charging_ma_max);
    if (rc < 0)
    {
        chip->batt_charging_ma_max = -EINVAL;
    }

    rc = of_property_read_u32_array(node,
            "qcom,batt-charging-ma-mitigation",
            chip->batt_chaging_ma_mitigation, BAT_TEMP_STATUS_MAX);
    if (rc)
    {
        pr_err("Couldn't read batt-charging-ma-mitigation limits rc = %d\n", rc);
    }

    rc = of_property_read_u32(node, "qcom,batt-trickle-charging-ma",
                            &chip->batt_trickle_charging_ma);
    if (rc < 0)
    {
        chip->batt_trickle_charging_ma = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,batt-trickle-to-cc-theshold-mv",
                            &chip->batt_trickle_to_cc_theshold_mv);
    if (rc < 0)
    {
        chip->batt_trickle_to_cc_theshold_mv = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,batt-uvlo-theshold-mv",
                            &chip->batt_uvlo_theshold_mv);
    if (rc < 0)
    {
        chip->batt_uvlo_theshold_mv = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,batt-auto-recharge-delta-mv",
                            &chip->batt_auto_recharge_delta_mv);
    if (rc < 0)
    {
        chip->batt_auto_recharge_delta_mv = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,batt-discharging-ma",
                            &chip->batt_discharging_ma);
    if (rc < 0)
    {
        chip->batt_discharging_ma = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,thermal-regulation-threshold",
                            &chip->thermal_regulation_threshold);
    if (rc < 0)
    {
        chip->thermal_regulation_threshold = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,batt-cc-chg-timer",
                            &chip->batt_cc_chg_timer);
    if (rc < 0)
    {
        chip->batt_cc_chg_timer = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,step-charging-batt-full-mv",
                            &chip->step_charging_batt_full_mv);
    if (rc < 0)
    {
        chip->step_charging_batt_full_mv = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,step-charging-current-ma",
                            &chip->step_charging_current_ma);
    if (rc < 0)
    {
        chip->step_charging_current_ma = -EINVAL;
    }

    rc = of_property_read_u32(node, "qcom,step-charging-delta-voltage-mv",
                            &chip->step_charging_delta_voltage_mv);
    if (rc < 0)
    {
        chip->step_charging_delta_voltage_mv = -EINVAL;
    }

    pr_info("bms-psy-name = %s, using-pmic-therm = %d\n",
                chip->bms_psy_name, chip->using_pmic_therm);
    pr_info("cold-batt-decidegc = %d, normal-state1-batt-decidegc = %d,\
            normal-state2-batt-decidegc = %d, normal-state3-batt-decidegc = %d,\
            hot-batt-decidegc = %d\n",
            chip->cold_batt_decidegc, chip->normal_state1_batt_decidegc,
            chip->normal_state2_batt_decidegc, chip->normal_state3_batt_decidegc,
            chip->hot_batt_decidegc);
    pr_info("batt-full-mv = %d, batt-full-terminate-ma = %d\n",
        chip->batt_full_mv, chip->batt_full_terminate_ma);
    pr_info("usb-input-ma = %d, usb-input-regulation-mv = %d\n",
        chip->usb_input_ma, chip->usb_input_regulation_mv);
    pr_info("batt-charging-ma-max = %d, batt-charging-ma-mitigation = <%d %d %d %d %d %d>\n",
        chip->batt_charging_ma_max,
        chip->batt_chaging_ma_mitigation[BAT_TEMP_STATUS_COLD],
        chip->batt_chaging_ma_mitigation[BAT_TEMP_STATUS_NORMAL_STATE1],
        chip->batt_chaging_ma_mitigation[BAT_TEMP_STATUS_NORMAL_STATE2],
        chip->batt_chaging_ma_mitigation[BAT_TEMP_STATUS_NORMAL_STATE3],
        chip->batt_chaging_ma_mitigation[BAT_TEMP_STATUS_NORMAL_STATE4],
        chip->batt_chaging_ma_mitigation[BAT_TEMP_STATUS_HOT]);
    pr_info("batt-trickle-charging-ma = %d, batt-trickle-to-cc-theshold-mv = %d\n",
        chip->batt_trickle_charging_ma, chip->batt_trickle_to_cc_theshold_mv);
    pr_info("batt-uvlo-theshold-mv = %d, batt-auto-recharge-delta-mv = %d\n",
        chip->batt_uvlo_theshold_mv, chip->batt_auto_recharge_delta_mv);
    pr_info("batt-discharging-ma = %d, thermal-regulation-threshold = %d\n",
        chip->batt_discharging_ma, chip->thermal_regulation_threshold);
    pr_info("qcom,batt-cc-chg-timer = %d\n", chip->batt_cc_chg_timer);
    pr_info("qcom,step-charging-batt-full-mv = %d\n", chip->step_charging_batt_full_mv);
    pr_info("qcom,step-charging-current-ma = %d\n", chip->step_charging_current_ma);
    pr_info("qcom,step-charging-delta-voltage-mv = %d\n", chip->step_charging_delta_voltage_mv);
    return 0;
}

static void dump_regs(struct mp2661_chg *chip)
{
    int rc;
    u8 reg;
    u8 addr;

    for (addr = INPUT_SOURCE_CTRL_REG; addr <= FAULT_REG; addr++)
    {
        rc = mp2661_read(chip, addr, &reg);
        if (rc < 0)
        {
            pr_err("Couldn't read 0x%02x rc = %d\n",
                        addr, rc);
        }
        else
        {
            pr_err("0x%02x = 0x%02x\n", addr, reg);
        }
    }
}

static int mp2661_show_regs(struct seq_file *m, void *data)
{
    struct mp2661_chg  *chip = m->private;
    dump_regs(chip);
    return 0;
}

static int mp2661_regs_debugfs_open(struct inode *inode, struct file *file)
{
    struct mp2661_chg  *chip = inode->i_private;

    return single_open(file, mp2661_show_regs, chip);
}

static const struct file_operations mp2661_regs_debugfs_ops = {
    .owner        = THIS_MODULE,
    .open        = mp2661_regs_debugfs_open,
    .read        = seq_read,
    .llseek        = seq_lseek,
    .release    = single_release,
};

static int create_debugfs_entries(struct mp2661_chg *chip)
{
    int rc;

    chip->debug_root = debugfs_create_dir("mp2661", NULL);
    if (!chip->debug_root)
    {
        pr_err("Couldn't create debug dir\n");
    }

    if (chip->debug_root)
    {
        struct dentry *ent;

        ent = debugfs_create_file("regs_data", S_IFREG | S_IRUGO,
                      chip->debug_root, chip,
                      &mp2661_regs_debugfs_ops);
        if (!ent || IS_ERR(ent))
        {
            rc = PTR_ERR(ent);
            pr_err(    "Couldn't create cnfg debug file rc = %d\n", rc);
        }
    }

    return 0;
}

#if 0
#define HYSTERESIS_DECIDEGC 20
static void mp2661_chg_adc_notification(enum qpnp_tm_state state, void *ctx)
{
    struct mp2661_chg *chip = ctx;
    int cur_batt_temp_status = 0;
    int temp = 0;

    if (state >= ADC_TM_STATE_NUM)
    {
        pr_err("invallid state parameter %d\n", state);
        return;
    }

    temp = mp2661_get_prop_batt_temp(chip);
    pr_info("temp = %d state = %s\n", temp,
                state == ADC_TM_WARM_STATE ? "hot" : "cold");

    if (ADC_TM_WARM_STATE == state)
    {
        if (temp >= chip->hot_batt_decidegc)
        {
            cur_batt_temp_status = BAT_TEMP_STATUS_HOT;

            chip->adc_param.low_temp =
                chip->hot_batt_decidegc - HYSTERESIS_DECIDEGC;
            chip->adc_param.state_request =
                ADC_TM_COOL_THR_ENABLE;
        }
        else if (temp >= chip->normal_state3_batt_decidegc)
        {
            cur_batt_temp_status = BAT_TEMP_STATUS_NORMAL_STATE4;

            chip->adc_param.low_temp =
                chip->normal_state3_batt_decidegc - HYSTERESIS_DECIDEGC;
            chip->adc_param.high_temp =
                chip->hot_batt_decidegc;
        }
        else if (temp >= chip->normal_state2_batt_decidegc)
        {
            cur_batt_temp_status = BAT_TEMP_STATUS_NORMAL_STATE3;

            chip->adc_param.low_temp =
                chip->normal_state2_batt_decidegc - HYSTERESIS_DECIDEGC;
            chip->adc_param.high_temp =
                chip->normal_state3_batt_decidegc;
        }
        else if (temp >= chip->normal_state1_batt_decidegc)
        {
            cur_batt_temp_status = BAT_TEMP_STATUS_NORMAL_STATE2;

            chip->adc_param.low_temp =
                chip->normal_state1_batt_decidegc - HYSTERESIS_DECIDEGC;
            chip->adc_param.high_temp =
                chip->normal_state2_batt_decidegc;
        }
        else if (temp >= chip->cold_batt_decidegc)
        {
            cur_batt_temp_status = BAT_TEMP_STATUS_NORMAL_STATE1;

            chip->adc_param.low_temp =
                chip->cold_batt_decidegc - HYSTERESIS_DECIDEGC;
            chip->adc_param.high_temp =
                        chip->normal_state1_batt_decidegc;
            chip->adc_param.state_request =
                    ADC_TM_HIGH_LOW_THR_ENABLE;
        }
    }
    else
    {
        if (temp <= chip->hot_batt_decidegc)
        {
            cur_batt_temp_status = BAT_TEMP_STATUS_NORMAL_STATE4;

            chip->adc_param.high_temp =
                            chip->hot_batt_decidegc + HYSTERESIS_DECIDEGC;
            chip->adc_param.low_temp =
                    chip->normal_state3_batt_decidegc;
            chip->adc_param.state_request =
                    ADC_TM_HIGH_LOW_THR_ENABLE;
        }
        else if (temp <= chip->normal_state3_batt_decidegc)
        {
            cur_batt_temp_status = BAT_TEMP_STATUS_NORMAL_STATE3;

            chip->adc_param.high_temp =
                chip->normal_state3_batt_decidegc + HYSTERESIS_DECIDEGC;
            chip->adc_param.low_temp =
                chip->normal_state2_batt_decidegc;
            chip->adc_param.state_request =
                ADC_TM_HIGH_LOW_THR_ENABLE;
        }
        else if (temp <= chip->normal_state2_batt_decidegc)
        {
            cur_batt_temp_status = BAT_TEMP_STATUS_NORMAL_STATE2;

            chip->adc_param.high_temp =
                chip->normal_state2_batt_decidegc + HYSTERESIS_DECIDEGC;
            chip->adc_param.low_temp =
                chip->normal_state1_batt_decidegc;
            chip->adc_param.state_request =
                ADC_TM_HIGH_LOW_THR_ENABLE;
        }
        else if (temp <= chip->normal_state1_batt_decidegc)
        {
            cur_batt_temp_status = BAT_TEMP_STATUS_NORMAL_STATE1;

            chip->adc_param.high_temp =
                chip->normal_state1_batt_decidegc + HYSTERESIS_DECIDEGC;
            chip->adc_param.low_temp =
                chip->cold_batt_decidegc;
            chip->adc_param.state_request =
                ADC_TM_HIGH_LOW_THR_ENABLE;
        }
        else if (temp <= chip->cold_batt_decidegc)
        {
            cur_batt_temp_status = BAT_TEMP_STATUS_COLD;

            chip->adc_param.high_temp =
                chip->cold_batt_decidegc + HYSTERESIS_DECIDEGC;
            chip->adc_param.low_temp =
                chip->cold_batt_decidegc;
            chip->adc_param.state_request =
                ADC_TM_HIGH_LOW_THR_ENABLE;
        }
    }

    if (cur_batt_temp_status ^ chip->batt_temp_status)
    {
        if (BAT_TEMP_STATUS_HOT == cur_batt_temp_status
            || BAT_TEMP_STATUS_COLD == cur_batt_temp_status)
        {
            mp2661_set_charging_enable(chip, false);
        }
        else
        {
            mp2661_set_charging_enable(chip, true);
            mp2661_set_appropriate_batt_charging_current(chip);
        }
        chip->batt_temp_status = cur_batt_temp_status;
    }

    pr_info("batt_temp_status = %d, low = %d deciDegC, high = %d deciDegC\n",
        chip->batt_temp_status,
        chip->adc_param.low_temp, chip->adc_param.high_temp);

    if (qpnp_adc_tm_channel_measure(chip->adc_tm_dev, &chip->adc_param))
    {
        pr_err("request ADC error\n");
    }
}
#endif

static void mp2661_initialize_batt_temp_status(struct mp2661_chg *chip)
{
    int temp = mp2661_get_prop_batt_temp(chip);

    if(temp >= chip->hot_batt_decidegc)
    {
        chip->batt_temp_status = BAT_TEMP_STATUS_HOT;
    }
    else if(temp >= chip->normal_state3_batt_decidegc)
    {
        chip->batt_temp_status = BAT_TEMP_STATUS_NORMAL_STATE4;
    }
    else if(temp >= chip->normal_state2_batt_decidegc)
    {
        chip->batt_temp_status = BAT_TEMP_STATUS_NORMAL_STATE3;
    }
    else if(temp >= chip->normal_state1_batt_decidegc)
    {
        chip->batt_temp_status = BAT_TEMP_STATUS_NORMAL_STATE2;
    }
    else if(temp >= chip->cold_batt_decidegc)
    {
        chip->batt_temp_status = BAT_TEMP_STATUS_NORMAL_STATE1;
    }
    else
    {
        chip->batt_temp_status = BAT_TEMP_STATUS_COLD;
    }

    chip->last_temp = temp;
    pr_info("temp = %d,chip->batt_temp_status = %d\n", temp, chip->batt_temp_status);
}

static int mp2661_hw_init(struct mp2661_chg *chip)
{
    int rc;

    rc = mp2661_set_batt_full_voltage(chip, chip->batt_full_mv);
    if (rc)
    {
        pr_err("Couldn't set charge full voltage rc = %d\n", rc);
    }

    rc = mp2661_set_batt_full_terminate_current(chip, chip->batt_full_terminate_ma);
    if (rc)
    {
        pr_err("Couldn't set batt full terminate current rc = %d\n", rc);
    }

    rc = mp2661_set_usb_input_current(chip, chip->usb_input_ma);
    if (rc)
    {
        pr_err("Couldn't set usb input current rc = %d\n", rc);
    }

    rc = mp2661_set_usb_input_voltage_regulation(chip, chip->usb_input_regulation_mv);
    if (rc)
    {
        pr_err("Couldn't set usb input voltage rc=%d\n", rc);
        return rc;
    }

    /*set charging current according to batt temp status */
    mp2661_set_appropriate_batt_charging_current(chip);

    rc = mp2661_set_batt_trickle_charging_current(chip, chip->batt_trickle_charging_ma);
    if (rc)
    {
        pr_err("Couldn't set batt trickle charging current rc=%d\n", rc);
        return rc;
    }

    rc = mp2661_set_pcb_otp_disable(chip, false);
    if (rc)
    {
        pr_err("Couldn't set pcb otp disable to false rc=%d\n", rc);
    }

    rc = mp2661_set_batt_trickle_to_cc_threshold(chip, chip->batt_trickle_to_cc_theshold_mv);
    if (rc)
    {
        pr_err("Couldn't set charge to cc threshold rc=%d\n", rc);
        return rc;
    }

    rc = mp2661_set_batt_uvlo_threshold(chip, chip->batt_uvlo_theshold_mv);
    if (rc)
    {
        pr_err("Couldn't set batt uvlo threshold rc=%d\n", rc);
        return rc;
    }

    rc = mp2661_set_batt_auto_recharge(chip, chip->batt_auto_recharge_delta_mv);
    if (rc)
    {
        pr_err("Couldn't set battery auto recharge rc=%d\n", rc);
        return rc;
    }

    rc = mp2661_set_batt_discharging_current(chip, chip->batt_discharging_ma);
    if (rc)
    {
        pr_err("Couldn't set batt discharging current rc=%d\n", rc);
        return rc;
    }

    rc = mp2661_set_thermal_regulation_threshold(chip, chip->thermal_regulation_threshold);
    if (rc)
    {
        pr_err("Couldn't set thermal regulation threshold rc=%d\n", rc);
        return rc;
    }

    rc = mp2661_en_bf_enable(chip, true);
    if (rc)
    {
        pr_err("Couldn't cset_en_bf rc=%d\n", rc);
        return rc;
    }

    rc = mp2661_set_ldo_fet_disconnect(chip, false);
    if (rc)
    {
        pr_err("Couldn't set ldo fet disconnect rc=%d\n", rc);
        return rc;
    }

    rc = mp2661_set_batt_fet_disconnect(chip, false);
    if (rc)
    {
        pr_err("Couldn't set batt fet disconnect rc=%d\n", rc);
        return rc;
    }

    /* TODO (b/30979364): The ntc temp of mp2661 is 60 and easily cause board to shutdown,
     * So disable ntc temporarily here but we need to re-enable it before product release.
    */
    rc = mp2661_ntc_enable(chip, true);
    if (rc)
    {
        pr_err("Couldn't enable ntc rc=%d\n", rc);
        return rc;
    }

    rc = mp2661_tmr2x_enable(chip, false);
    if (rc)
    {
        pr_err("Couldn't enable tmr2x rc=%d\n", rc);
        return rc;
    }

    /*disable watchdog_timer*/
    rc = mp2661_set_i2c_watchdog_timer(chip, false);
    if (rc)
    {
        pr_err("Couldn't set i2c watchdog timer rc=%d\n", rc);
        return rc;
    }

    /* enable safety timer */
    rc = mp2661_safety_timer_enable(chip, true);
    if (rc)
    {
        pr_err("Couldn't enable safety timer rc=%d\n", rc);
    }

    /* set cc chg timer */
    rc = mp2661_set_cc_chg_timer(chip, chip->batt_cc_chg_timer);
    if (rc)
    {
        pr_err("Couldn't set cc chg timer rc=%d\n", rc);
    }

    if (BAT_TEMP_STATUS_HOT == chip->batt_temp_status
            || BAT_TEMP_STATUS_COLD == chip->batt_temp_status)
    {
        rc = mp2661_set_charging_enable(chip, false);
    }
    else
    {
        rc = mp2661_set_charging_enable(chip, true);
    }
    if (rc)
    {
        pr_err("Couldn't set charging enable rc=%d\n", rc);
        return rc;
    }

    return 0;
}

#if 0
static void mp2661_initialize_qpnp_adc_tm_btm(struct mp2661_chg *chip)
{
    int rc;

    if (BAT_TEMP_STATUS_HOT == chip->batt_temp_status)
    {
        chip->adc_param.low_temp = chip->hot_batt_decidegc;
        chip->adc_param.state_request =
            ADC_TM_LOW_THR_ENABLE;
    }
    else if (BAT_TEMP_STATUS_NORMAL_STATE4 == chip->batt_temp_status)
    {
        chip->adc_param.low_temp = chip->normal_state3_batt_decidegc;
        chip->adc_param.high_temp = chip->hot_batt_decidegc;
        chip->adc_param.state_request =
                    ADC_TM_HIGH_LOW_THR_ENABLE;
    }
    else if (BAT_TEMP_STATUS_NORMAL_STATE3 == chip->batt_temp_status)
    {
        chip->adc_param.low_temp = chip->normal_state2_batt_decidegc;
        chip->adc_param.high_temp = chip->normal_state3_batt_decidegc;
        chip->adc_param.state_request =
                    ADC_TM_HIGH_LOW_THR_ENABLE;
    }
    else if (BAT_TEMP_STATUS_NORMAL_STATE2 == chip->batt_temp_status)
    {
        chip->adc_param.low_temp = chip->normal_state1_batt_decidegc;
        chip->adc_param.high_temp = chip->normal_state2_batt_decidegc;
        chip->adc_param.state_request =
                    ADC_TM_HIGH_LOW_THR_ENABLE;
    }
    else if (BAT_TEMP_STATUS_NORMAL_STATE1 == chip->batt_temp_status)
    {
        chip->adc_param.low_temp = chip->cold_batt_decidegc;
        chip->adc_param.high_temp = chip->normal_state1_batt_decidegc;
        chip->adc_param.state_request =
                    ADC_TM_HIGH_LOW_THR_ENABLE;
    }
    else if (BAT_TEMP_STATUS_COLD == chip->batt_temp_status)
    {
        chip->adc_param.high_temp = chip->normal_state1_batt_decidegc;
        chip->adc_param.state_request = ADC_TM_HIGH_THR_ENABLE;
    }

    chip->adc_param.timer_interval = ADC_MEAS2_INTERVAL_1S;
    chip->adc_param.btm_ctx = chip;
    chip->adc_param.threshold_notification =
            mp2661_chg_adc_notification;
    chip->adc_param.channel = P_MUX2_1_1;

    rc = qpnp_adc_tm_channel_measure(chip->adc_tm_dev,
                            &chip->adc_param);
    if (rc)
    {
        pr_err("requesting ADC error %d\n", rc);
    }
}
#endif

/*writable properties*/
static int mp2661_batt_property_is_writeable(struct power_supply *psy,
                enum power_supply_property psp)
{
    switch (psp)
    {
        case POWER_SUPPLY_PROP_USB_INPUT_CURRENT:
            return 1;
        case POWER_SUPPLY_PROP_CHARGING_ENABLED:
            return 1;
        default:
            break;
    }

    return 0;
}

static void mp2661_check_and_update_charging_voltage_current(struct mp2661_chg *chip,
                           int full_voltage_mv, int charging_current_ma)
{
    int rc;

    if (!chip)
    {
        return;
    }

    if(chip->batt_full_now_mv != full_voltage_mv)
    {
        rc = mp2661_set_batt_full_voltage(chip, full_voltage_mv);
        if (rc)
        {
            pr_err("Couldn't set charge full voltage rc = %d\n", rc);
        }
    }

    if(chip->batt_charging_current_now_ma != charging_current_ma)
    {
        rc = mp2661_set_batt_charging_current(chip, charging_current_ma);
        if (rc)
        {
            pr_err("Couldn't set batt charging current rc = %d\n", rc);
        }
    }
}

#define CONSECUTIVE_COUNT                           3
static void mp2661_adjust_batt_charging_current_and_voltage(
                struct mp2661_chg *chip)
{
    int vbatt_mv = 0;
    int current_now_ma = 0;
    int batt_voltage_threshold_mv = 0;
    int rc = -1;
    static int count = 0;
    static bool step_charging_flag = false;

    if((1 == chip->usb_present) && (BAT_TEMP_STATUS_NORMAL_STATE4 == chip->batt_temp_status))
    {
        vbatt_mv = mp2661_get_prop_battery_voltage_now(chip) / 1000;
        pr_debug("battery voltage is %dmv\n", vbatt_mv);
        batt_voltage_threshold_mv = (chip->step_charging_batt_full_mv - chip->step_charging_delta_voltage_mv);
        if(vbatt_mv <= batt_voltage_threshold_mv)
        {
            /* update batt full voltage and charging current */
            mp2661_check_and_update_charging_voltage_current(chip,
                    chip->step_charging_batt_full_mv,
                    chip->batt_chaging_ma_mitigation[BAT_TEMP_STATUS_NORMAL_STATE4]);

            count = 0;
            step_charging_flag = false;
        }
        else if(vbatt_mv <= chip->step_charging_batt_full_mv)
        {
            if(!step_charging_flag)
            {
                /* update batt full voltage and charging current */
                mp2661_check_and_update_charging_voltage_current(chip,
                        chip->step_charging_batt_full_mv,
                        chip->batt_chaging_ma_mitigation[BAT_TEMP_STATUS_NORMAL_STATE4]);

                if(POWER_SUPPLY_STATUS_FULL != chip->charging_status)
                {
                    current_now_ma = mp2661_get_prop_current_now(chip) / 1000;
                    if(current_now_ma <= chip->step_charging_current_ma)
                    {
                        count++;
                        pr_debug("count is %d\n", count);
                        if(CONSECUTIVE_COUNT == count)
                        {
                            pr_info("count equals to max value(%d)\n", CONSECUTIVE_COUNT);
                            count = 0;

                            if(vbatt_mv > (chip->step_charging_batt_full_mv - chip->step_charging_delta_voltage_mv / 2))
                            {
                                /* update batt full voltage and charging current */
                                mp2661_check_and_update_charging_voltage_current(chip,
                                       chip->batt_full_mv,
                                       chip->step_charging_current_ma);
                                step_charging_flag = true;
                            }
                        }
                    }
                    else
                    {
                        count = 0;
                        step_charging_flag = false;
                    }
                }
                else
                {
                    count = 0;
                    /* update batt full voltage and charging current */
                    mp2661_check_and_update_charging_voltage_current(chip,
                            chip->batt_full_mv,
                            chip->step_charging_current_ma);
                    step_charging_flag = true;
                    /* reset charging enable action */
                    rc = mp2661_set_charging_enable(chip, false);
                    if (rc)
                    {
                        pr_err("Couldn't reset charging disable rc=%d\n", rc);
                    }

                    mdelay(200);

                    rc = mp2661_set_charging_enable(chip, true);
                    if (rc)
                    {
                        pr_err("Couldn't reset charging enable rc=%d\n", rc);
                    }
                }
            }
        }
        else
        {
            /* update batt full voltage and charging current */
            mp2661_check_and_update_charging_voltage_current(chip,
                    chip->batt_full_mv,
                    chip->step_charging_current_ma);

            count = 0;
            step_charging_flag = false;
        }
    }
    else
    {
        count = 0;
        step_charging_flag = false;
    }
}

#define MONITOR_WORK_DELAY_MS         10000
#define MONITOR_TEMP_DELTA            10
#define AP_MASK_RX_GPIO_TEMP          450
#define AP_MASK_RX_GPIO_TEMP_DELTA    30
extern void idtp9220_ap_mask_rxint_enable(bool enable);
static __ref int mp2661_monitor_kthread(void *arg)
{
    int temp;
    int last_batt_temp_status;
    struct mp2661_chg *chip = (struct mp2661_chg *)arg;
    struct sched_param param = {.sched_priority = MAX_RT_PRIO - 1};

    sched_setscheduler(current, SCHED_FIFO, &param);
    pr_info("enter mp2661 monitor thread\n");

    while(1)
    {
        get_monotonic_boottime(&chip->last_monitor_time);
        temp = mp2661_get_prop_batt_temp(chip);
        pr_info("temp = %d\n",temp);

        if(abs(temp - chip->last_temp) >= MONITOR_TEMP_DELTA)
        {
            pr_err("temp = %d, last_temp = %d\n", temp, chip->last_temp);

            last_batt_temp_status = chip->batt_temp_status;

            mp2661_initialize_batt_temp_status(chip);

            if(chip->batt_temp_status != last_batt_temp_status)
            {
                if (BAT_TEMP_STATUS_HOT == chip->batt_temp_status
                   || BAT_TEMP_STATUS_COLD == chip->batt_temp_status)
                {
                    mp2661_set_charging_enable(chip, false);
                }
                else if(BAT_TEMP_STATUS_HOT == last_batt_temp_status
                    || BAT_TEMP_STATUS_COLD == last_batt_temp_status)
                {
                    mp2661_set_charging_enable(chip, true);
                    mp2661_set_appropriate_batt_charging_current(chip);
                }
                else
                {
                    mp2661_set_appropriate_batt_charging_current(chip);
                }
            }
        }

        mp2661_adjust_batt_charging_current_and_voltage(chip);

        if(down_timeout(&chip->monitor_temp_sem, msecs_to_jiffies(MONITOR_WORK_DELAY_MS)))
        {
            pr_debug("Unable to acquire monitor temp lock\n");
        }
    }

    return 0;
}

static int mp2661_charger_probe(struct i2c_client *client,
                const struct i2c_device_id *id)
{
    int rc;
    struct mp2661_chg *chip;
    struct power_supply *usb_psy;

    usb_psy = power_supply_get_by_name("usb");
    if (!usb_psy)
    {
        pr_err("USB psy not found; deferring probe\n");
        return -EPROBE_DEFER;
    }

    chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
    if (!chip)
    {
        pr_err("Couldn't allocate memory\n");
        return -ENOMEM;
    }

    chip->client = client;
    chip->dev = &client->dev;
    chip->usb_psy = usb_psy;
    chip->fake_battery_soc = -EINVAL;

    /* early for VADC get, defer probe if needed */
    chip->vadc_dev = qpnp_get_vadc(chip->dev, "chg");
    if (IS_ERR(chip->vadc_dev))
    {
        rc = PTR_ERR(chip->vadc_dev);
        if (rc != -EPROBE_DEFER)
        {
            pr_err("vadc property missing\n");
        }
        return rc;
    }

    mutex_init(&chip->read_write_lock);
    device_init_wakeup(chip->dev, true);

    rc = mp2661_parse_dt(chip);
    if (rc < 0)
    {
        pr_err("Couldn't parse DT nodes rc=%d\n", rc);
        return rc;
    }

#if 0
    /* using adc_tm for implementing pmic therm */
    if (chip->using_pmic_therm)
    {
        chip->adc_tm_dev = qpnp_get_adc_tm(chip->dev, "chg");
        if (IS_ERR(chip->adc_tm_dev))
        {
            rc = PTR_ERR(chip->adc_tm_dev);
            if (rc != -EPROBE_DEFER)
            {
                pr_err("adc_tm property missing\n");
            }
            return rc;
        }
    }
#endif

    mp2661_initialize_batt_temp_status(chip);

    rc = mp2661_hw_init(chip);
    if (rc)
    {
        pr_err("Couldn't intialize hardware rc=%d\n", rc);
        return rc;
    }

    i2c_set_clientdata(client, chip);

    chip->batt_psy.name        = "battery";
    chip->batt_psy.type        = POWER_SUPPLY_TYPE_BATTERY;
    chip->batt_psy.get_property    = mp2661_battery_get_property;
    chip->batt_psy.set_property    = mp2661_battery_set_property;
    chip->batt_psy.properties    = mp2661_battery_properties;
    chip->batt_psy.num_properties  = ARRAY_SIZE(mp2661_battery_properties);
    chip->batt_psy.external_power_changed = mp2661_external_power_changed;
    chip->batt_psy.supplied_to    = pm_batt_supplied_to;
    chip->batt_psy.num_supplicants    = ARRAY_SIZE(pm_batt_supplied_to);
    chip->batt_psy.property_is_writeable = mp2661_batt_property_is_writeable;

    rc = power_supply_register(chip->dev, &chip->batt_psy);
    if (rc < 0)
    {
        pr_err("Unable to register batt_psy rc = %d\n", rc);
        return rc;
    }

    INIT_WORK(&chip->process_interrupt_work, mp2661_process_interrupt_work);

    /* stat irq configuration */
    if (client->irq)
    {
        mp2661_chg_stat_handler(client->irq, chip);
        rc = devm_request_threaded_irq(&client->dev, client->irq, NULL,
                mp2661_chg_stat_handler,
                IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
                "mp2661_chg_stat_irq", chip);
        if (rc < 0)
        {
            pr_err(    "request_irq for irq=%d  failed rc = %d\n", client->irq, rc);
            goto unregister_batt_psy;
        }
        enable_irq_wake(client->irq);
    }

#if 0
    if (chip->using_pmic_therm)
    {
        mp2661_initialize_qpnp_adc_tm_btm(chip);
    }
#endif


    create_debugfs_entries(chip);

    global_mp2661 = chip;

    chip->monitor_temp_task = kthread_create(mp2661_monitor_kthread, global_mp2661,
        "monitor_temp");
    if (IS_ERR(chip->monitor_temp_task))
    {
        pr_err("can not creat monitor temp threthd\n");
    }
    else
    {
        sema_init(&chip->monitor_temp_sem, 1);
        wake_up_process(chip->monitor_temp_task);
    }

    return 0;
unregister_batt_psy:
    power_supply_unregister(&chip->batt_psy);

    return rc;
}

static int mp2661_charger_remove(struct i2c_client *client)
{
    struct mp2661_chg *chip = i2c_get_clientdata(client);

    debugfs_remove_recursive(chip->debug_root);
    power_supply_unregister(&chip->batt_psy);
    if (!IS_ERR(chip->monitor_temp_task))
    {
        kthread_stop(chip->monitor_temp_task);
    }

    return 0;
}

static int mp2661_suspend(struct device *dev)
{
    return 0;
}

static int mp2661_suspend_noirq(struct device *dev)
{
    return 0;
}

static int mp2661_resume(struct device *dev)
{
     struct i2c_client *client = to_i2c_client(dev);
     struct mp2661_chg *chip = i2c_get_clientdata(client);

     if(!chip->usb_present && !chip->ap_mask_rx_int_gpio)
     {
         return 0;
     }

     get_monotonic_boottime(&chip->resume_time);
     pr_info("mp2661 resume_time = %ld, last_monitor_time =%ld\n",
            chip->resume_time.tv_sec, chip->last_monitor_time.tv_sec);
     if( (chip->resume_time.tv_sec - chip->last_monitor_time.tv_sec) >
             MONITOR_WORK_DELAY_MS / 1000)
     {
          up(&chip->monitor_temp_sem);
     }

     return 0;
}

static const struct dev_pm_ops mp2661_pm_ops = {
    .resume        = mp2661_resume,
    .suspend_noirq    = mp2661_suspend_noirq,
    .suspend    = mp2661_suspend,
};

static const struct i2c_device_id mp2661_charger_id[] = {
    {"mp2661-charger", 0},
    {},
};
MODULE_DEVICE_TABLE(i2c, mp2661_charger_id);

static struct i2c_driver mp2661_charger_driver = {
    .driver        = {
        .name        = "mp2661-charger",
        .owner        = THIS_MODULE,
        .of_match_table    = mp2661_match_table,
        .pm        = &mp2661_pm_ops,
    },
    .probe        = mp2661_charger_probe,
    .remove        = mp2661_charger_remove,
    .id_table    = mp2661_charger_id,
};

module_i2c_driver(mp2661_charger_driver);

MODULE_DESCRIPTION("mp2661 Charger");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("i2c:mp2661-charger");
