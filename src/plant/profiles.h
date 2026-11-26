/* Machine profiles: the same control loop, telemetry and agent run any of these simulated machines. */
#ifndef FM_PROFILES_H
#define FM_PROFILES_H

#include "../common/fm_shm.h"

enum fm_aux2_kind { AUX2_LOAD = 0, AUX2_DRAIN, AUX2_INTEGRAL };
enum fm_action { ACT_WARN = 0, ACT_DERATE, ACT_STOP };

/* Pseudo-signals a rule can watch besides the six real ones. */
#define FM_SIG_EST_DEV 100  /* |model-estimated PV - measured PV|: catches sensor faults */
#define FM_SIG_TRACK   101  /* |effective setpoint - measured PV|: tracking error */

struct fm_rule {
    const char *code;
    const char *title;
    int sig;          /* enum fm_sig or FM_SIG_* */
    char op;          /* '>' or '<' */
    float threshold;
    int hold_ms;      /* condition must persist this long */
    int action;       /* enum fm_action */
    float derate;     /* setpoint multiplier while derated */
};

struct fm_scenario {
    const char *name;
    const char *description;
    float load_extra;   /* added to the load / disturbance input */
    float aux1_extra;   /* added to the aux1 target (temperature, pressure) */
    float bias_per_s;   /* measured-PV bias growth (sensor drift) */
    float aux2_set;     /* if >= 0, force aux2 to this value (e.g. battery state of charge) */
    float ramp_s;       /* seconds to ramp load/aux1 extras in */
};

struct fm_profile {
    const char *name;
    const char *machine;
    struct fm_signal_info sig[FM_NSIG];
    float sp;                    /* nominal setpoint */
    float gain, tau_pv;          /* pv_target = gain*out - load_coef*load */
    float load_coef;
    float kp, ki;                /* PI controller, output in % */
    float cur_base, cur_gain;    /* current = base + gain*out*(1 + load/100) */
    float aux1_base, aux1_k, tau_aux1; /* aux1_target = base + k*current + scenario extra */
    int aux2_kind;
    float aux2_init, aux2_rate;  /* LOAD: nominal %, DRAIN: % per A per s, INTEGRAL: pv/3600 per s */
    const struct fm_rule *rules;
    int n_rules;
    const struct fm_scenario *scenarios;
    int n_scenarios;
};

const struct fm_profile *fm_profile_find(const char *name);
const char *fm_profile_list(void);

#endif
