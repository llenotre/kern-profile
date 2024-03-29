// This plugin has been written to support QEMU version 8.2.0
// Using other versions might break it

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>
#include <glib.h>

#include <qemu-plugin.h>

// Hard limit to the stack depth to observe
#define MAX_DEPTH	64

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

struct ctx
{
	// The FD of the output file
	int out_fd;
	// The delay between each sample to be collected in microseconds
	suseconds_t sample_delay;
	// The timestamp of the next sample to collect
	struct timeval next_sample_ts;
};

static struct ctx ctx;

// The usage of internal QEMU functions are hacks addressing the limitations of
// TCG plugins. Even though the public API is allowed to change from one version
// to another, those are even more prone to break

// Internal QEMU function. Returns the CPU with the given ID.
extern void *qemu_get_cpu(int index);
// Internal QEMU function. Allows to read or write guest memory.
extern int cpu_memory_rw_debug(void *cpu, uint64_t addr,
                               void *ptr, uint64_t len, bool is_write);

// Returns the value of the register with the given ID.
static uint64_t get_cpu_register_val(void *cpu, unsigned int id)
{
	/*
	 * Registers are not directly accessible, so we need a hack.
	 * Under i386, the CPU structure looks like this:
	 *
	 * struct ArchCPU {
	 *	CPUState parent_obj;
	 *
	 *	CPUX86State env;
	 *	// ...
	 * };
	 *
	 * The CPUX86State structure looks like this:
	 *
	 * typedef struct CPUArchState {
	 * 	target_ulong regs[CPU_NB_REGS];
	 *	// ...
	 * } CPUX86State;
	 *
	 * Standard registers are located in the array above.
	 *
	 * Tip: In C, you can get the size of a type at compile time by using:
	 * https://stackoverflow.com/a/35261673
	 */

	// TODO adapt size to architecture
	const size_t REGS_OFF = 10176;
	const size_t REG_SIZE = 4;
	return *(uint32_t *) (cpu + REGS_OFF + id * REG_SIZE);
}

// This is used as a clock to perform sampling
static void vcpu_insn_exec(unsigned int cpu_index, void *eip)
{
	// If the delay isn't expired, ignore
	struct timeval tv;
	gettimeofday(&tv, NULL);
	if (tv.tv_sec < ctx.next_sample_ts.tv_sec
		|| (tv.tv_sec == ctx.next_sample_ts.tv_sec
		&& tv.tv_usec < ctx.next_sample_ts.tv_usec))
		return;
	ctx.next_sample_ts = tv;
	ctx.next_sample_ts.tv_sec += ctx.sample_delay / 1000000;
	ctx.next_sample_ts.tv_usec += ctx.sample_delay % 1000000;

	// The sample delay has expired. Read the stack and write it to the output file

	// Get registers
	void *cpu = qemu_get_cpu(cpu_index);
	uint64_t ebp = get_cpu_register_val(cpu, 5);

	// Iterate through stack
	uint64_t frames_buf[MAX_DEPTH];
	frames_buf[0] = (uint64_t) eip;
	uint8_t i;
	for (i = 1; i < MAX_DEPTH; ++i)
	{
		char buf[4]; // TODO 64 bits

		// TODO do only one read in memory

		// Get function address (return address on the stack)
		// TODO 64 bits
		int err = cpu_memory_rw_debug(cpu, ebp + 4, buf, sizeof(buf), 0);
		if (err)
			break;
		frames_buf[i] = *(uint32_t *) &buf[0];
		// If reached the end of the stack (exclude code outside of the kernel)
		if (frames_buf[i] <= (uint64_t) 0xc0000000) // TODO 64 bits
			break;

		// Get next frame
		err = cpu_memory_rw_debug(cpu, ebp, buf, sizeof(buf), 0);
		if (err)
			break;
		ebp = *(uint32_t *) &buf[0];
	}

	// Build iovec
	struct iovec v[2];
	// Frames count
	v[0].iov_base = (void *) &i;
	v[0].iov_len = sizeof(i);
	// Frames
	v[1].iov_base = (void *) &frames_buf[0];
	v[1].iov_len = i * sizeof(uint64_t);

	// TODO loop until everything is written
	errno = 0;
	writev(ctx.out_fd, v, 2);
	if (errno)
		dprintf(STDERR_FILENO, "warning: could not write to output file: %s\n", strerror(errno));
}

// Executed each time a block of instructions is translated
static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    size_t i;
    for (i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
		uint64_t vaddr = qemu_plugin_insn_vaddr(insn);
		qemu_plugin_register_vcpu_insn_exec_cb(
			insn, vcpu_insn_exec, QEMU_PLUGIN_CB_NO_REGS,
			GUINT_TO_POINTER(vaddr));
    }
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
	close(ctx.out_fd);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
	// Default values
	char *out_path = "qemu-profile";
	suseconds_t sample_delay = 10;
	// Parse arguments
	for (size_t i = 0; i < argc; ++i)
	{
        char *name = argv[i];
        char *name_end = strchr(name, '=');
		char *val = name_end + 1;
		*name_end = '\0';
		if (g_strcmp0(name, "out") == 0)
			out_path = val;
		else if (g_strcmp0(name, "delay") == 0)
			sample_delay = atoi(val);
		else
		{
			dprintf(STDERR_FILENO, "invalid argument: %s\n", name);
			return -1;
		}
	}

	// Open output file
	errno = 0;
	ctx.out_fd = open(out_path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
	if (errno)
	{
		dprintf(STDERR_FILENO, "qemu: %s: %s", out_path, strerror(errno));
		return 1;
	}

	// Init timing
	ctx.sample_delay = sample_delay;
	gettimeofday(&ctx.next_sample_ts, NULL);

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
	return 0;
}
