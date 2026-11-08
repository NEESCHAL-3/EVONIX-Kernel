#define STEP_JEITA_TUPLE_COUNT	6
#define THERMAL_LIMIT_COUNT	16
#define THERMAL_LIMIT_TUPLE	6

#define FCC_DESCENT_DELAY	1000
#define JEITA_FCC_DESCENT_STEP	1000
#define SW_CV_COUNT		3

#define TYPEC_BURN_TEMP_THRESHOLD	650
#define TYPEC_NTC_TEMP_ACCELERATION	4
#define TYPEC_NTC_TEMP_LOW_THRESHOLD	350
#define TYPEC_NTC_TEMP_RECOVER_THRESHOLD	450
#define TYPEC_NTC_TEMP_COMBINED_THRESHOLD	600
#define TYPEC_NTC_TEMP_FITTING_THRESHOLD	50000

#define MAX_THERMAL_FCC		22000

#define MIN_THERMAL_FCC		200
#define ITERM_FCC_WARM		9000
#define ENABLE_FCC_ITERM_LEVEL	14
#define WARM_TEMP        	481

struct step_jeita_cfg0 {
	int low_threshold;
	int high_threshold;
	int value;
};

struct step_jeita_cfg1 {
	int low_threshold;
	int high_threshold;
	int extra_threshold;
	int low_value;
	int high_value;
};

void reset_step_jeita_charge(struct mtk_charger *info);
void start_dfx_report_timer(struct mtk_charger *info);
int step_jeita_init(struct mtk_charger *info, struct device *dev);
