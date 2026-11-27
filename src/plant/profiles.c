/*
 * Simulated machine profiles. All three are demonstrations with plausible dynamics and fictional fault
 * codes; they are not models of any real product. The medical profile in particular is a simulation for
 * demonstrating software isolation and is not a medical device.
 */
#include "profiles.h"
#include <string.h>

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ---- Industrial: packaging line PL-3, conveyor drive ---- */
static const struct fm_rule industrial_rules[] = {
    {"W11", "Motor temperature high", FM_AUX1, '>', 85.0f, 2000, ACT_WARN, 1.0f},
    {"E17", "Motor over-temperature: line speed derated to 60%", FM_AUX1, '>', 95.0f, 2000, ACT_DERATE, 0.6f},
    {"E42", "Infeed jam: drive stopped (motor current limit)", FM_CURRENT, '>', 22.0f, 300, ACT_STOP, 0.0f},
    {"W05", "Conveyor overload", FM_AUX2, '>', 75.0f, 1000, ACT_WARN, 1.0f},
    {"E08", "Speed sensor disagrees with drive model", FM_SIG_EST_DEV, '>', 100.0f, 1500, ACT_WARN, 1.0f},
};
static const struct fm_scenario industrial_scen[] = {
    {"jam", "Product jams at the infeed: load surges until the drive hits its current limit", 75, 0, 0, -1, 3},
    {"cooling_fail", "Motor cooling fan fails: winding temperature climbs over ~30 s", 0, 50, 0, -1, 20},
    {"sensor_drift", "Speed encoder drifts: measured speed reads progressively high", 0, 0, 9, -1, 0},
    {"overload", "Heavier product batch: conveyor load rises to ~80%", 40, 0, 0, -1, 10},
};

/* ---- Automotive: electric-vehicle drive unit ---- */
static const struct fm_rule automotive_rules[] = {
    {"W-200", "Inverter temperature high", FM_AUX1, '>', 95.0f, 2000, ACT_WARN, 1.0f},
    {"T-201", "Inverter over-temperature: limp mode, torque limited to 40%", FM_AUX1, '>', 105.0f, 2000, ACT_DERATE, 0.4f},
    {"B-310", "Low battery: power limited to 50%", FM_AUX2, '<', 15.0f, 1000, ACT_DERATE, 0.5f},
    {"S-115", "Motor position sensor fault: safe torque-off", FM_SIG_EST_DEV, '>', 25.0f, 800, ACT_STOP, 0.0f},
};
static const struct fm_scenario automotive_scen[] = {
    {"coolant_pump_fail", "Coolant pump fails: inverter temperature climbs over ~30 s", 0, 55, 0, -1, 20},
    {"low_battery", "Battery state of charge drops to 14%", 0, 0, 0, 14, 0},
    {"sensor_fault", "Motor position sensor drifts: measured torque diverges", 0, 0, 7, -1, 0},
};

/* ---- Medical (simulation only): infusion pump IP-2 ---- */
static const struct fm_rule medical_rules[] = {
    {"W3", "Downstream line pressure rising", FM_AUX1, '>', 200.0f, 1000, ACT_WARN, 1.0f},
    {"A3", "Downstream occlusion: infusion stopped", FM_AUX1, '>', 300.0f, 1000, ACT_STOP, 0.0f},
    {"A5", "Air-in-line / flow sensor mismatch: infusion stopped", FM_SIG_EST_DEV, '>', 30.0f, 500, ACT_STOP, 0.0f},
};
static const struct fm_scenario medical_scen[] = {
    {"occlusion", "Tubing kinks downstream: line pressure climbs over ~15 s", 0, 600, 0, -1, 15},
    {"air_in_line", "Air bubble passes the flow sensor: measured flow drops away from delivered flow", 0, 0, -45, -1, 0},
};

static const struct fm_profile profiles[] = {
    {"industrial", "Packaging line PL-3, conveyor drive (simulated)",
     {{"speed_setpoint", "rpm"}, {"motor_speed", "rpm"}, {"drive_output", "%"},
      {"motor_temp", "degC"}, {"conveyor_load", "%"}, {"motor_current", "A"}},
     1450, 25, 0.25f, 6, 0.08f, 0.9f, 2, 0.12f, 25, 3.0f, 12, AUX2_LOAD, 40, 0,
     industrial_rules, N(industrial_rules), industrial_scen, N(industrial_scen), 0},
    {"automotive", "EV drive unit DU-1, traction motor and inverter (simulated)",
     {{"torque_request", "Nm"}, {"motor_torque", "Nm"}, {"inverter_duty", "%"},
      {"inverter_temp", "degC"}, {"battery_soc", "%"}, {"phase_current", "A"}},
     180, 3.0f, 0.08f, 0, 0.3f, 3.0f, 5, 3.2f, 30, 0.2f, 12, AUX2_DRAIN, 62, 0.00002f,
     automotive_rules, N(automotive_rules), automotive_scen, N(automotive_scen), 0},
    {"medical", "Infusion pump IP-2 (simulation only, not a medical device)",
     {{"flow_setpoint", "mL/h"}, {"measured_flow", "mL/h"}, {"pump_motor", "%"},
      {"line_pressure", "mmHg"}, {"volume_infused", "mL"}, {"motor_current", "mA"}},
     125, 2.5f, 0.5f, 0, 0.2f, 1.5f, 40, 2.0f, 50, 0.25f, 4, AUX2_INTEGRAL, 0, 1.0f / 3600.0f,
     medical_rules, N(medical_rules), medical_scen, N(medical_scen), 1},
};

const struct fm_profile *fm_profile_find(const char *name) {
    for (int i = 0; i < N(profiles); i++)
        if (strcmp(profiles[i].name, name) == 0) return &profiles[i];
    return NULL;
}

const char *fm_profile_list(void) { return "industrial, automotive, medical"; }
