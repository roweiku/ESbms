#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>

#include <bms/bms.h>
#include <bms/bms_common.h>

#include "bms_state_machine.h"
#include "button.h"

LOG_MODULE_REGISTER(bms_sm, CONFIG_LOG_DEFAULT_LEVEL);

/* List of events */
struct k_event sm_events; /* Shared with button.c */

/* Forward declaration of state table */
static const struct smf_state smf_state_table[];

/* Charger/charge detection thresholds */
#define CHARGER_V_THRESHOLD 0.5f /* V above total_voltage to detect charger */
#define CHARGE_I_DETECT     0.3f /* A: current threshold to detect active charging */
#define CHARGE_I_CUTOFF     0.1f /* A: current below which charge is considered complete */
#define CELL_V_BALANCE_MARGIN \
    0.05f                       /* V: margin below cell_chg_voltage_limit to trigger balance \
                                 */
#define CHARGER_ABSENT_CYCLES 5 /* Poll cycles (~2.5s) to confirm charger removal */
#define CHARGER_DETECT_CYCLES 6 /* Poll cycles (3s) to confirm charger/current detection */
#define BALANCE_TIMEOUT_MS    (10 * 60 * 1000)

/* User defined object */
struct bms_sm_obj
{
    struct smf_ctx ctx; /* Must be first member */
    int64_t start_time;
    struct bms_context *bms;
    uint32_t events;
    uint8_t detect_count; /* Shared debounce counter for detection logic */
} sm_obj;

/* State change event — posted when SM leaf state changes */
struct k_event sm_state_events;

/*
 * Hardware error check — called at start of parent run handlers.
 * Returns true if error detected (transition to SM_ERROR already requested).
 */
static inline bool check_hw_error(struct bms_sm_obj *o)
{
    if (o->bms->error_flags != 0) {
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_ERROR]);
        return true;
    }
    return false;
}

/* ── SM_STANDBY (L1 parent: user OFF) ───────────────────────── */

static void standby_entry(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    o->bms->dis_enable = false;
    // o->bms->chg_enable = false;
    LOG_INF("Entered STANDBY");
}

static enum smf_state_result standby_run(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;

    if (check_hw_error(o)) {
        return SMF_EVENT_HANDLED;
    }

    /* Button press → SOC display (only propagated from SM_OFF) */
    if (o->events & BTN_EVT_PRESS) {
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_SOC]);
        return SMF_EVENT_HANDLED;
    }

    if (o->bms->dis_enable && o->bms->chg_enable) {
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_NORMAL]);
        return SMF_EVENT_HANDLED;
    }

    return SMF_EVENT_HANDLED;
}

/* ── SM_OFF (STANDBY child, initial: all FETs off) ──────────── */

static void off_entry(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    bms_ic_set_switches(o->bms->ic_dev, 0);
    o->detect_count = 0;
    o->bms->state = BMS_STATE_OFF;
    LOG_INF("Entered OFF");
}

static enum smf_state_result off_run(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    struct bms_context *bms = o->bms;
    float ext_v = bms->ic_data.external_voltage;
    float total_v = bms->ic_data.total_voltage;
    float pack_ov = bms->ic_conf.cell_ov_limit * bms->ic_data.connected_cells;

    if (ext_v > pack_ov) {
        // TODO: ws2812 warning
        LOG_WRN("External OV: %.2fV (limit %.2fV)", (double)ext_v, (double)pack_ov);
    }
    else if (ext_v > total_v + CHARGER_V_THRESHOLD) {
        o->detect_count++;
        if (o->detect_count >= CHARGER_DETECT_CYCLES && o->bms->chg_enable == true) {
            smf_set_state(SMF_CTX(o), &smf_state_table[SM_CHARGE]);
            return SMF_EVENT_HANDLED;
        }
    }
    else {
        o->detect_count = 0;
    }

    /* Propagate button events to STANDBY parent */
    return SMF_EVENT_PROPAGATE;
}

/* ── SM_SOC (STANDBY child: SOC display) ─────────────────────── */

static void soc_entry(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    o->start_time = k_uptime_get();
    LOG_INF("Entered SOC");
}

static enum smf_state_result soc_run(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;

    if (o->events & BTN_EVT_PRESS) {
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_OPEN]);
    }
    else if (o->events & BTN_EVT_RELEASE) {
        o->start_time = k_uptime_get();
    }
    else if (k_uptime_get() - o->start_time > 3000) {
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_OFF]);
    }

    return SMF_EVENT_HANDLED;
}

/* ── SM_OPEN (STANDBY child: button held, pending power-on) ─── */

static void open_entry(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    o->start_time = k_uptime_get();
    LOG_INF("Entered OPEN");
}

static enum smf_state_result open_run(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    int64_t elapsed = k_uptime_get() - o->start_time;

    if (elapsed <= 750 && (o->events & BTN_EVT_RELEASE)) {
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_SOC]);
    }
    else if (elapsed > 1250) {
        if (o->bms->chg_enable) {
            smf_set_state(SMF_CTX(o), &smf_state_table[SM_NORMAL]);
        }
        else {
            smf_set_state(SMF_CTX(o), &smf_state_table[SM_SOC]);
        }
    }

    return SMF_EVENT_HANDLED;
}

/* ── SM_ON (L1 parent: user ON, all FETs enabled) ───────────── */

static void on_entry(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    o->bms->dis_enable = true;
    // o->bms->chg_enable = true;
    LOG_INF("Entered ON");
}

static enum smf_state_result on_run(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;

    if (check_hw_error(o)) {
        return SMF_EVENT_HANDLED;
    }

    if (!(o->bms->dis_enable && o->bms->chg_enable)) {
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_OFF]);
        return SMF_EVENT_HANDLED;
    }

    if (o->events & BTN_EVT_PRESS) {
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_C1]);
        return SMF_EVENT_HANDLED;
    }

    return SMF_EVENT_HANDLED;
}

/* ── SM_NORMAL (ON child, initial: all FETs on) ─────────────── */

static void normal_entry(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    bms_ic_set_switches(o->bms->ic_dev,
                        BMS_SWITCH_CHG | BMS_SWITCH_PCHG | BMS_SWITCH_DIS | BMS_SWITCH_PDSG);
    o->detect_count = 0;
    o->bms->state = BMS_STATE_NORMAL;
    LOG_INF("Entered NORMAL");
}

static enum smf_state_result normal_run(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    /* Detect charging activity → start charging session */
    if (o->bms->ic_data.current > CHARGE_I_DETECT) {
        o->detect_count++;
        if (o->detect_count >= CHARGER_DETECT_CYCLES) {
            smf_set_state(SMF_CTX(o), &smf_state_table[SM_CHARGE]);
            return SMF_EVENT_HANDLED;
        }
    }
    else {
        o->detect_count = 0;
    }

    /* Propagate button events and hw_error check to ON parent */
    return SMF_EVENT_PROPAGATE;
}

/* ── SM_C1 (ON child: shutdown confirm step 1) ──────────────── */

static void c1_entry(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    o->start_time = k_uptime_get();
    LOG_INF("Entered C1");
}

static enum smf_state_result c1_run(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;

    if (o->events & BTN_EVT_PRESS) {
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_C2]);
    }
    else if (o->events & BTN_EVT_RELEASE) {
        o->start_time = k_uptime_get();
    }
    else if (k_uptime_get() - o->start_time > 3000) {
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_NORMAL]);
    }

    return SMF_EVENT_HANDLED;
}

/* ── SM_C2 (ON child: shutdown confirm step 2) ──────────────── */

static void c2_entry(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    o->start_time = k_uptime_get();
    LOG_INF("Entered C2");
}

static enum smf_state_result c2_run(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    int64_t elapsed = k_uptime_get() - o->start_time;

    if (elapsed <= 750 && (o->events & BTN_EVT_RELEASE)) {
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_C1]);
    }
    else if (elapsed > 1250) {
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_OFF]);
    }

    return SMF_EVENT_HANDLED;
}

/* ── SM_CHARGING (L1 parent: charging session) ──────────────── */

static void charging_entry(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    o->detect_count = 0;

    // o->bms->chg_enable = true;
    // o->bms->dis_enable = true;
    LOG_INF("Entered CHARGING");
}

static enum smf_state_result charging_run(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;

    if (check_hw_error(o)) {
        return SMF_EVENT_HANDLED;
    }

    if (!o->bms->chg_enable) {
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_OFF]);
        return SMF_EVENT_HANDLED;
    }

    /* Charger removal detection with debounce */
    if (o->bms->ic_data.external_voltage > o->bms->ic_data.total_voltage + CHARGER_V_THRESHOLD
        || o->bms->ic_data.current > CHARGE_I_DETECT)
    {
        o->detect_count = 0;
    }
    else {
        o->detect_count++;
        if (o->detect_count >= CHARGER_ABSENT_CYCLES) {
            LOG_INF("Charger removed, exiting charging session");
            if (SMF_CTX(o)->previous == &smf_state_table[SM_NORMAL]) {
                smf_set_state(SMF_CTX(o), &smf_state_table[SM_NORMAL]);
            }
            else {
                smf_set_state(SMF_CTX(o), &smf_state_table[SM_OFF]);
            }
            return SMF_EVENT_HANDLED;
        }
    }

    /* Consume all button events during charging */
    if (o->events & BTN_EVT_ALL) {
        return SMF_EVENT_HANDLED;
    }

    return SMF_EVENT_HANDLED;
}

/* ── SM_CHARGE (CHARGING child, initial: all FETs on) ────────── */

static void charge_entry(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    bms_ic_set_switches(o->bms->ic_dev,
                        BMS_SWITCH_CHG | BMS_SWITCH_PCHG | BMS_SWITCH_DIS | BMS_SWITCH_PDSG);
    o->bms->state = BMS_STATE_NORMAL;
    LOG_INF("Entered CHARGE");
}

static enum smf_state_result charge_run(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    struct bms_context *bms = o->bms;
    float cell_v_max = bms->ic_data.cell_voltage_max;
    float cell_chg_limit = bms->ic_conf.cell_chg_voltage_limit;
    float cell_v_diff = cell_v_max - bms->ic_data.cell_voltage_min;

    /* Full detection: any cell reached limit or charge current tapered off */
    if (cell_v_max >= cell_chg_limit) {
        LOG_INF("Charge complete: cell_voltage_max %.3fV >= limit %.3fV", (double)cell_v_max,
                (double)cell_chg_limit);
        bms->full = true;
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_OFF]);
        return SMF_EVENT_HANDLED;
    }

    if (cell_v_max > (cell_chg_limit - CELL_V_BALANCE_MARGIN)
        && bms->ic_data.current < CHARGE_I_CUTOFF)
    {
        LOG_INF("Charge complete: current %.3fA < cutoff, cell_max %.3fV",
                (double)bms->ic_data.current, (double)cell_v_max);
        bms->full = true;
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_OFF]);
        return SMF_EVENT_HANDLED;
    }

    /* Balance needed: cell approaching limit with voltage imbalance */
    if (cell_v_max > (cell_chg_limit - CELL_V_BALANCE_MARGIN)
        && cell_v_diff > bms->ic_conf.bal_cell_voltage_diff + 0.02f)
    {
        LOG_INF("Balance needed: cell_max %.3fV, diff %.3fV", (double)cell_v_max,
                (double)cell_v_diff);
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_BALANCE]);
        return SMF_EVENT_HANDLED;
    }

    /* Propagate to SM_CHARGING parent for hw_error, charger removal, button handling */
    return SMF_EVENT_PROPAGATE;
}

/* ── SM_BALANCE (CHARGING child: CHG off for balancing) ──────── */

static void balance_entry(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    bms_ic_set_switches(o->bms->ic_dev, 0);
    o->start_time = k_uptime_get();
    o->bms->state = BMS_STATE_OFF;
    LOG_INF("Entered BALANCE");
}

static enum smf_state_result balance_run(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    struct bms_context *bms = o->bms;
    float cell_v_diff = bms->ic_data.cell_voltage_max - bms->ic_data.cell_voltage_min;

    /* Cells balanced enough or voltage relaxed — resume charging */
    if (cell_v_diff <= bms->ic_conf.bal_cell_voltage_diff) {
        LOG_INF("Balance complete: diff %.3fV <= threshold %.3fV", (double)cell_v_diff,
                (double)bms->ic_conf.bal_cell_voltage_diff);
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_CHARGE]);
        return SMF_EVENT_HANDLED;
    }

    if (k_uptime_get() - o->start_time > BALANCE_TIMEOUT_MS) {
        LOG_INF("Balance timeout, resuming charge");
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_CHARGE]);
        return SMF_EVENT_HANDLED;
    }

    /* Propagate to SM_CHARGING parent for hw_error, charger removal, button handling */
    return SMF_EVENT_PROPAGATE;
}

/* ── SM_SHUTDOWN (L1 leaf: shutdown sequence) ────────────────── */

static void shutdown_entry(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    bms_ic_set_switches(o->bms->ic_dev, 0);
    o->bms->dis_enable = false;
    o->bms->chg_enable = false;
    o->bms->state = BMS_STATE_SHUTDOWN;
    LOG_INF("Entered SHUTDOWN");
}

static enum smf_state_result shutdown_run(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    bms_ic_set_mode(o->bms->ic_dev, BMS_IC_MODE_OFF);
    return SMF_EVENT_HANDLED;
}

/* ── SM_ERROR (L1 leaf: hardware error, all FETs off) ────────── */

static void err_entry(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;
    bms_ic_set_switches(o->bms->ic_dev, 0);
    o->bms->dis_enable = false;
    o->bms->chg_enable = false;
    LOG_ERR("Entered ERROR, flags: 0x%08x", o->bms->error_flags);
}

static enum smf_state_result err_run(void *obj)
{
    struct bms_sm_obj *o = (struct bms_sm_obj *)obj;

    if (o->bms->ic_data.error_flags == 0 && (o->events & BTN_EVT_LONG_PRESS_5S)) {
        o->bms->error_flags = 0;
        o->bms->chg_enable = true;
        smf_set_state(SMF_CTX(o), &smf_state_table[SM_OFF]);
        LOG_INF("Exiting ERROR: flags cleared + long press");
    }

    return SMF_EVENT_HANDLED;
}

/* ── State table ─────────────────────────────────────────────── */

static const struct smf_state smf_state_table[SM_STATE_COUNT] = {
    /* L1 parents */
    [SM_STANDBY] =
        SMF_CREATE_STATE(standby_entry, standby_run, NULL, NULL, &smf_state_table[SM_OFF]),
    [SM_ON] = SMF_CREATE_STATE(on_entry, on_run, NULL, NULL, &smf_state_table[SM_NORMAL]),
    [SM_CHARGING] =
        SMF_CREATE_STATE(charging_entry, charging_run, NULL, NULL, &smf_state_table[SM_CHARGE]),

    /* STANDBY children */
    [SM_OFF] = SMF_CREATE_STATE(off_entry, off_run, NULL, &smf_state_table[SM_STANDBY], NULL),
    [SM_SOC] = SMF_CREATE_STATE(soc_entry, soc_run, NULL, &smf_state_table[SM_STANDBY], NULL),
    [SM_OPEN] = SMF_CREATE_STATE(open_entry, open_run, NULL, &smf_state_table[SM_STANDBY], NULL),

    /* ON children */
    [SM_NORMAL] = SMF_CREATE_STATE(normal_entry, normal_run, NULL, &smf_state_table[SM_ON], NULL),
    [SM_C1] = SMF_CREATE_STATE(c1_entry, c1_run, NULL, &smf_state_table[SM_ON], NULL),
    [SM_C2] = SMF_CREATE_STATE(c2_entry, c2_run, NULL, &smf_state_table[SM_ON], NULL),

    /* CHARGING children */
    [SM_CHARGE] =
        SMF_CREATE_STATE(charge_entry, charge_run, NULL, &smf_state_table[SM_CHARGING], NULL),
    [SM_BALANCE] =
        SMF_CREATE_STATE(balance_entry, balance_run, NULL, &smf_state_table[SM_CHARGING], NULL),

    /* L1 leaf states */
    [SM_SHUTDOWN] = SMF_CREATE_STATE(shutdown_entry, shutdown_run, NULL, NULL, NULL),
    [SM_ERROR] = SMF_CREATE_STATE(err_entry, err_run, NULL, NULL, NULL),
};

/* ── State machine thread ────────────────────────────────────── */

static void bms_sm_thread(void *p1, void *p2, void *p3)
{
    sm_obj.bms = (struct bms_context *)p1;

    k_event_init(&sm_events);

    k_event_init(&sm_state_events);

    smf_set_initial(SMF_CTX(&sm_obj), &smf_state_table[SM_STANDBY]);

    LOG_INF("BMS state machine started");

    while (true) {
        uint32_t evt = k_event_wait(&sm_events, BTN_EVT_ALL, true, K_MSEC(500));
        sm_obj.events = evt;

        const struct smf_state *prev = sm_obj.ctx.current;
        int ret = smf_run_state(SMF_CTX(&sm_obj));
        if (ret) {
            LOG_ERR("State machine terminated: %d", ret);
            break;
        }
        if (sm_obj.ctx.current != prev) {
            uint8_t state = (uint8_t)(sm_obj.ctx.current - smf_state_table);
            // DEBUG
            // LOG_INF("SM: set event BIT(%d) now", state);
            k_event_set(&sm_state_events, BIT(state));
            // LOG_INF("SM: set event BIT(%d) done", state);
        }
    }
}

extern struct bms_context bms;
K_THREAD_DEFINE(bms_sm_tid, 1024, bms_sm_thread, &bms, NULL, NULL, 5, 0, 0);
