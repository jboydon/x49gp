/* $Id: main.c,v 1.30 2008/12/11 12:18:17 ecd Exp $
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

#include <gtk/gtk.h>
#include <glib.h>

#include <x49gp.h>
#include <x49gp_ui.h>
#include <memory.h>
#include <s3c2410.h>
#include <s3c2410_power.h>
#include <s3c2410_timer.h>
#include <x49gp_timer.h>

#include "gdbstub.h"

static x49gp_t *x49gp;

/* LD TEMPO HACK */
CPUState *__GLOBAL_env;

int semihosting_enabled = 1;

uint8_t *phys_ram_base;
int phys_ram_size;
ram_addr_t ram_size = 0x80000; // LD ???

/* vl.c */
int singlestep;

#if !(defined(__APPLE__) || defined(_POSIX_C_SOURCE) && !defined(__sun__))
static void *oom_check(void *ptr)
{
    if (ptr == NULL) {
        abort();
    }
    return ptr;
}
#endif

void *qemu_memalign(size_t alignment, size_t size)
{
#if defined(__APPLE__) || defined(_POSIX_C_SOURCE) && !defined(__sun__)
    int ret;
    void *ptr;
    ret = posix_memalign(&ptr, alignment, size);
    if (ret != 0)
        abort();
    return ptr;
#elif defined(CONFIG_BSD)
    return oom_check(valloc(size));
#else
    return oom_check(memalign(alignment, size));
#endif
}


void qemu_init_vcpu(void *_env)
{
    CPUState *env = _env;

    env->nr_cores = 1;
    env->nr_threads = 1;
}

int qemu_cpu_self(void *env)
{
    return 1;
}

void qemu_cpu_kick(void *env)
{
}

void armv7m_nvic_set_pending(void *opaque, int irq)
{
  abort();
}
int armv7m_nvic_acknowledge_irq(void *opaque)
{
  abort();
}
void armv7m_nvic_complete_irq(void *opaque, int irq)
{
  abort();
}

void *
qemu_malloc(size_t size)
{
	return malloc(size);
}

void *
qemu_mallocz(size_t size)
{
	void *ptr;

	ptr = qemu_malloc(size);
	if (NULL == ptr)
		return NULL;
	memset(ptr, 0, size);
	return ptr;
}

void
qemu_free(void *ptr)
{
	free(ptr);
}

void *
qemu_vmalloc(size_t size)
{
#if defined(__linux__)
	void *mem;
	if (0 == posix_memalign(&mem, sysconf(_SC_PAGE_SIZE), size))
		return mem;
	return NULL;
#else
	return valloc(size);
#endif
}

#define SWI_Breakpoint 0x180000

uint32_t
do_arm_semihosting(CPUState *env)
{
	uint32_t number;
	if (env->thumb) {
		number = lduw_code(env->regs[15] - 2) & 0xff;
	} else {
		number = ldl_code(env->regs[15] - 4) & 0xffffff;
	}
	switch (number) {
	case SWI_Breakpoint:
		break;

	case 0:
#ifdef DEBUG_X49GP_SYSCALL
		printf("%s: SWI LR %08x: syscall %u: args %08x %08x %08x %08x %08x %08x %08x\n",
			__FUNCTION__, env->regs[14], env->regs[0],
			env->regs[1], env->regs[2], env->regs[3],
			env->regs[4], env->regs[5], env->regs[6],
			env->regs[7]);
#endif

#if 1
		switch (env->regs[0]) {
		case 305:	/* Beep */
			printf("%s: BEEP: frequency %u, time %u, override %u\n",
				__FUNCTION__, env->regs[1], env->regs[2], env->regs[3]);

			gdk_beep();
			env->regs[0] = 0;
			return 1;

		case 28:	/* CheckBeepEnd */
			env->regs[0] = 0;
			return 1;

		case 29:	/* StopBeep */
			env->regs[0] = 0;
			return 1;

		default:
			break;
		}
#endif
		break;

	default:
		break;
	}

	return 0;
}

void
x49gp_set_idle(x49gp_t *x49gp, x49gp_arm_idle_t idle)
{
#ifdef DEBUG_X49GP_ARM_IDLE
	if (idle != x49gp->arm_idle) {
		printf("%s: arm_idle %u, idle %u\n", __FUNCTION__, x49gp->arm_idle, idle);
	}
#endif

	x49gp->arm_idle = idle;

	if (x49gp->arm_idle == X49GP_ARM_RUN) {
		x49gp->env->halted = 0;
	} else {
		x49gp->env->halted = 1;
		cpu_exit(x49gp->env);
	}
}

static void
arm_sighnd(int sig)
{
	switch (sig) {
	case SIGUSR1:
//		stop_simulator = 1;
//		x49gp->arm->CallDebug ^= 1;
		break;
	default:
		fprintf(stderr, "%s: sig %u\n", __FUNCTION__, sig);
		break;
	}
}

void
x49gp_gtk_timer(void *data)
{
	while (gtk_events_pending()) {
// printf("%s: gtk_main_iteration_do()\n", __FUNCTION__);
		gtk_main_iteration_do(FALSE);
	}

	x49gp_mod_timer(x49gp->gtk_timer,
			x49gp_get_clock() + X49GP_GTK_REFRESH_INTERVAL);
}

void
x49gp_lcd_timer(void *data)
{
	x49gp_t *x49gp = data;
	int64_t now, expires;

// printf("%s: lcd_update\n", __FUNCTION__);
	x49gp_lcd_update(x49gp);
	gdk_flush();

	now = x49gp_get_clock();
	expires = now + X49GP_LCD_REFRESH_INTERVAL;

// printf("%s: now: %lld, next update: %lld\n", __FUNCTION__, now, expires);
	x49gp_mod_timer(x49gp->lcd_timer, expires);
}

struct options {
	char *config;
	int debug_port;
	int start_debugger;
	char *firmware;
	x49gp_reinit_t reinit;

	int more_options;
};

struct option_def;

typedef int (*option_action)(struct options *opt, struct option_def *match,
			     char *this_opt, char *param, char *progname);

struct option_def {
	option_action action;
	char *longname;
	char shortname;
};

static int action_help(struct options *opt, struct option_def *match,
		       char *this_opt, char *param, char *progname);
static int action_debuglater(struct options *opt, struct option_def *match,
			     char *this_opt, char *param, char *progname);
static int action_debug(struct options *opt, struct option_def *match,
			char *this_opt, char *param, char *progname);
static int action_reinit_flash(struct options *opt, struct option_def *match,
			      char *this_opt, char *param, char *progname);
static int action_reinit_flash_full(struct options *opt,
				    struct option_def *match, char *this_opt,
				    char *param, char *progname);
static int action_reboot(struct options *opt, struct option_def *match,
			 char *this_opt, char *param, char *progname);

static int action_unknown_with_param(struct options *opt,
				     struct option_def *match, char *this_opt,
				     char *param, char *progname);
static int action_longopt(struct options *opt, struct option_def *match,
			  char *this_opt, char *param, char *progname);
static int action_endopt(struct options *opt, struct option_def *match,
			 char *this_opt, char *param, char *progname);

struct option_def option_defs[] = {
	{ action_help, "help", 'h' },
	{ action_debuglater, "enable-debug", 'D' },
	{ action_debug, "debug", 'd' },
	{ action_reinit_flash, "reflash", 'f' },
	{ action_reinit_flash_full, "reflash-full", 'F' },
	{ action_reboot, "reboot", 'r' },

	{ action_longopt, NULL, '-' },
	{ action_unknown_with_param, NULL, '=' },
	{ action_endopt, "", '\0' }
};

static void
warn_unneeded_param(struct option_def *match, char *this_opt)
{
	if (this_opt[1] == '-') {
		fprintf(stderr, "The option \"--%s\" does not support"
			" parameters\n", match->longname);
	} else
		fprintf(stderr, "The option '-%c' does not support parameters\n",
			match->shortname);
}

static int
action_help(struct options *opt, struct option_def *match, char *this_opt,
	    char *param, char *progname)
{
	if (param != NULL)
		warn_unneeded_param(match, this_opt);

	fprintf(stderr, "Emulator for HP 49G+ / 50G calculators\n"
		"Usage: %s [<options>] [<config-file>]\n"
		"Valid options:\n"
		" -D, --enable-debug[=<port]    enable the debugger interface\n"
		"                               (default port: %u)\n"
		" -d, --debug[=<port>]          like -D, but also start the"
		" debugger immediately\n"
		" -f, --reflash[=firmware]      rebuild the flash using the"
		" supplied firmware\n"
		"                               (default: select one"
		" interactively)\n"
		"                               (implies -r for safety"
		" reasons)\n"
		" -F, --reflash-full[=firmware] like -f, but don't preserve the"
		" flash contents\n"
		"                               in the area beyond the"
		" firmware\n"
		" -r, --reboot                  reboot on startup instead of"
		" continuing from the\n"
		"                               saved state in the config"
		" file\n"
		" -h, --help                    print this message and exit\n"
		"The config file is formatted as INI file and contains the"
		" settings for which\n"
		"persistence makes sense, like calculator model, CPU"
		" registers, etc.\n"
		"If the config file is omitted, ~/.%s/config is used.\n"
		"Please consult the manual for more details on config file"
		" settings.\n", progname, DEFAULT_GDBSTUB_PORT, progname);
	exit(0);
}

static int
action_debuglater(struct options *opt, struct option_def *match, char *this_opt,
				  char *param, char *progname)
{
	char *end;
	int port;

	if (param == NULL) {
		if (opt->debug_port == 0)
			opt->debug_port = DEFAULT_GDBSTUB_PORT;
		return FALSE;
	}

	port = strtoul(param, &end, 0);
	if ((end == param) || (*end != '\0')) {
		fprintf(stderr, "Invalid port \"%s\", using default\n", param);
		if (opt->debug_port == 0)
			opt->debug_port = DEFAULT_GDBSTUB_PORT;
		return TRUE;
	}

	if (opt->debug_port != 0 && opt->debug_port != DEFAULT_GDBSTUB_PORT)
		fprintf(stderr, "Additional debug port \"%s\" specified,"
			" overriding\n", param);
	opt->debug_port = port;
	return TRUE;
}

static int
action_debug(struct options *opt, struct option_def *match, char *this_opt,
			 char *param, char *progname)
{
	opt->start_debugger = TRUE;
	return action_debuglater(opt, match, this_opt, param, progname);
}

static int
action_reinit_flash(struct options *opt, struct option_def *match,
		    char *this_opt, char *param, char *progname)
{
	if (opt->reinit < X49GP_REINIT_FLASH)
		opt->reinit = X49GP_REINIT_FLASH;

	if (param == NULL)
		return FALSE;

	if (opt->firmware != NULL)
		fprintf(stderr, "Additional firmware file \"%s\" specified,"
		" overriding\n", param);
	opt->firmware = param;
	return TRUE;
}

static int
action_reinit_flash_full(struct options *opt,
			 struct option_def *match, char *this_opt,
			 char *param, char *progname)
{
	int result = action_reinit_flash(opt, match, this_opt, param, progname);
	opt->reinit = X49GP_REINIT_FLASH_FULL;
	return result;
}

static int
action_reboot(struct options *opt, struct option_def *match, char *this_opt,
	      char *param, char *progname)
{
	if (param != NULL)
		warn_unneeded_param(match, this_opt);

	if (opt->reinit < X49GP_REINIT_REBOOT_ONLY)
		opt->reinit = X49GP_REINIT_REBOOT_ONLY;
	return param != NULL;
}

static int
action_longopt(struct options *opt, struct option_def *match, char *this_opt,
	       char *param, char *progname)
{
	int i;
	char *test_str, *option_str;

	if (this_opt[1] != '-' || param != NULL) {
		fprintf(stderr, "Unrecognized option '-', ignoring\n");
		return FALSE;
	}

	for (i = 0; i < sizeof(option_defs) / sizeof(option_defs[0]); i++) {
		if (option_defs[i].longname == NULL)
			continue;

		test_str = option_defs[i].longname;
		option_str = this_opt + 2;

		while (*test_str != '\0' && *test_str == *option_str) {
			test_str++;
			option_str++;
		}

		if (*test_str != '\0') continue;

		switch (*option_str) {
		case '\0':
			(option_defs[i].action)(opt, option_defs + i, this_opt,
						NULL, progname);
			return TRUE;
		case '=':
			(option_defs[i].action)(opt, option_defs + i, this_opt,
						option_str+2, progname);
			return TRUE;
		}
	}

	fprintf(stderr, "Unrecognized option \"%s\", ignoring\n", this_opt + 2);
	return TRUE;
}

static int
action_unknown_with_param(struct options *opt, struct option_def *match,
			  char *this_opt, char *param, char *progname)
{
	return TRUE;
}

static int
action_endopt(struct options *opt, struct option_def *match, char *this_opt,
	      char *param, char *progname)
{
	opt->more_options = FALSE;
	return TRUE;
}

static void
parse_shortopt(struct options *opt, char *this_opt, char *progname)
{
	char *option = this_opt + 1;
	char *param;
	int i;

	if (*option == '\0') {
		fprintf(stderr,
			"Empty option present, ignoring\n");
		return;
	}

	do {
		for (i = 0; i < sizeof(option_defs) / sizeof(option_defs[0]);
		     i++) {

			if (*option == option_defs[i].shortname) {
				if (*(option + 1) == '=') {
					param = option + 2;
				} else {
					param = NULL;
				}

				if ((option_defs[i].action)(opt, option_defs + i,
							    this_opt, param,
							    progname))
					return;
				break;
			}
		}


		if (i == sizeof(option_defs) / sizeof(option_defs[0]))
			fprintf(stderr,
				"Unrecognized option '%c', ignoring\n",
				*option);
		option++;
	} while (*option != '\0');
}

static void
parse_options(struct options *opt, int argc, char **argv, char *progname)
{
	opt->more_options = TRUE;

	while (argc > 1) {
		switch (argv[1][0]) {
			case '\0':
				break;
			break;

			case '-':
				if (opt->more_options) {
					parse_shortopt(opt, argv[1], progname);
					break;
				}
				/* FALL THROUGH */

			default:
				if (opt->config != NULL) {
					fprintf(stderr,
						"Additional config file \"%s\""
						" specified, overriding\n",
						argv[1]);
				}
				opt->config = argv[1];
		}

		argc--;
		argv++;
	}

}

void
ui_sighnd(int sig)
{
	switch (sig) {
	case SIGINT:
	case SIGQUIT:
	case SIGTERM:
		x49gp->arm_exit = 1;
		cpu_exit(x49gp->env);
		break;
	}
}

int
main(int argc, char **argv)
{
	char *progname, *progpath;
	int error;
	struct options opt;
	const char *home;


	progname = g_path_get_basename(argv[0]);
	progpath = g_path_get_dirname(argv[0]);


	gtk_init(&argc, &argv);


	opt.config = NULL;
	opt.debug_port = 0;
	opt.start_debugger = FALSE;
	opt.reinit = X49GP_REINIT_NONE;
	opt.firmware = NULL;
	parse_options(&opt, argc, argv, progname);

	x49gp = malloc(sizeof(x49gp_t));
	if (NULL == x49gp) {
		fprintf(stderr, "%s: %s:%u: Out of memory\n",
			progname, __FUNCTION__, __LINE__);
		exit(1);
	}
	memset(x49gp, 0, sizeof(x49gp_t));

#ifdef DEBUG_X49GP_MAIN
	fprintf(stderr, "_SC_PAGE_SIZE: %08lx\n", sysconf(_SC_PAGE_SIZE));

	printf("%s:%u: x49gp: %p\n", __FUNCTION__, __LINE__, x49gp);
#endif

	INIT_LIST_HEAD(&x49gp->modules);


	x49gp->progname = progname;
	x49gp->progpath = progpath;
	x49gp->clk_tck = sysconf(_SC_CLK_TCK);

	x49gp->emulator_fclk = 75000000;
	x49gp->PCLK_ratio = 4;
	x49gp->PCLK = 75000000 / 4;

	//cpu_set_log(0xffffffff);
	cpu_exec_init_all(0);
	x49gp->env = cpu_init("arm926");
	__GLOBAL_env = x49gp->env;

//	cpu_set_log(cpu_str_to_log_mask("all"));

	x49gp_timer_init(x49gp);

	x49gp->gtk_timer = x49gp_new_timer(X49GP_TIMER_REALTIME,
					   x49gp_gtk_timer, x49gp);
	x49gp->lcd_timer = x49gp_new_timer(X49GP_TIMER_VIRTUAL,
					   x49gp_lcd_timer, x49gp);

	x49gp_ui_init(x49gp);

	x49gp_s3c2410_arm_init(x49gp);

	x49gp_flash_init(x49gp);
	x49gp_sram_init(x49gp);

	x49gp_s3c2410_init(x49gp);

	if (x49gp_modules_init(x49gp)) {
		exit(1);
	}

	if (opt.config == NULL) {
		char config_dir[strlen(progname) + 2];

		home = g_get_home_dir();
		sprintf(config_dir, ".%s", progname);
		opt.config = g_build_filename(home, config_dir,
					      "config", NULL);
	}

	x49gp->basename = g_path_get_dirname(opt.config);
	x49gp->debug_port = opt.debug_port;
	x49gp->startup_reinit = opt.reinit;
	x49gp->firmware = opt.firmware;

	error = x49gp_modules_load(x49gp, opt.config);
	if (error || opt.reinit >= X49GP_REINIT_REBOOT_ONLY) {
		if (error && error != -EAGAIN) {
			exit(1);
		}
		x49gp_modules_reset(x49gp, X49GP_RESET_POWER_ON);
	}
// x49gp_modules_reset(x49gp, X49GP_RESET_POWER_ON);

	signal(SIGINT, ui_sighnd);
	signal(SIGTERM, ui_sighnd);
	signal(SIGQUIT, ui_sighnd);

	signal(SIGUSR1, arm_sighnd);


	x49gp_set_idle(x49gp, 0);

// stl_phys(0x08000a1c, 0x55555555);


	x49gp_mod_timer(x49gp->gtk_timer, x49gp_get_clock());
	x49gp_mod_timer(x49gp->lcd_timer, x49gp_get_clock());


	if(opt.debug_port != 0 && opt.start_debugger) {
		gdbserver_start(opt.debug_port);
		gdb_handlesig(x49gp->env, 0);
	}

	x49gp_main_loop(x49gp);


	x49gp_modules_save(x49gp, opt.config);
	x49gp_modules_exit(x49gp);


#if 0
	printf("ClkTicks: %lu\n", ARMul_Time(x49gp->arm));
	printf("D TLB: hit0 %lu, hit1 %lu, search %lu (%lu), walk %lu\n",
		x49gp->mmu->dTLB.hit0, x49gp->mmu->dTLB.hit1,
		x49gp->mmu->dTLB.search, x49gp->mmu->dTLB.nsearch,
		x49gp->mmu->dTLB.walk);
	printf("I TLB: hit0 %lu, hit1 %lu, search %lu (%lu), walk %lu\n",
		x49gp->mmu->iTLB.hit0, x49gp->mmu->iTLB.hit1,
		x49gp->mmu->iTLB.search, x49gp->mmu->iTLB.nsearch,
		x49gp->mmu->iTLB.walk);
#endif
	return 0;
}
