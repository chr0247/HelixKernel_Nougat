/*
 * Based on arch/arm/kernel/setup.c
 *
 * Copyright (C) 1995-2001 Russell King
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/utsname.h>
#include <linux/initrd.h>
#include <linux/console.h>
#include <linux/cache.h>
#include <linux/bootmem.h>
#include <linux/seq_file.h>
#include <linux/screen_info.h>
#include <linux/init.h>
#include <linux/kexec.h>
#include <linux/crash_dump.h>
#include <linux/root_dev.h>
#include <linux/clk-provider.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/smp.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/memblock.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/of_platform.h>
#include <linux/efi.h>
#include <linux/personality.h>
#include <linux/dma-mapping.h>

#include <asm/fixmap.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/elf.h>
#include <asm/cputable.h>
#include <asm/cpufeature.h>
#include <asm/cpu_ops.h>
#include <asm/kasan.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/smp_plat.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/traps.h>
#include <asm/memblock.h>
#include <asm/psci.h>
#include <asm/efi.h>

unsigned int processor_id;
EXPORT_SYMBOL(processor_id);

unsigned long elf_hwcap __read_mostly;
EXPORT_SYMBOL_GPL(elf_hwcap);

char* (*arch_read_hardware_id)(void);
EXPORT_SYMBOL(arch_read_hardware_id);

#ifdef CONFIG_COMPAT
#define COMPAT_ELF_HWCAP_DEFAULT	\
				(COMPAT_HWCAP_HALF|COMPAT_HWCAP_THUMB|\
				 COMPAT_HWCAP_FAST_MULT|COMPAT_HWCAP_EDSP|\
				 COMPAT_HWCAP_TLS|COMPAT_HWCAP_VFP|\
				 COMPAT_HWCAP_VFPv3|COMPAT_HWCAP_VFPv4|\
				 COMPAT_HWCAP_NEON|COMPAT_HWCAP_IDIV|\
				 COMPAT_HWCAP_LPAE)
unsigned int compat_elf_hwcap __read_mostly = COMPAT_ELF_HWCAP_DEFAULT;
unsigned int compat_elf_hwcap2 __read_mostly;
#endif

DECLARE_BITMAP(cpu_hwcaps, ARM64_NCAPS);

unsigned int boot_reason;
EXPORT_SYMBOL(boot_reason);

unsigned int cold_boot;
EXPORT_SYMBOL(cold_boot);

static const char *cpu_name;
static const char *machine_name;
phys_addr_t __fdt_pointer __initdata;

static struct resource mem_res[] = {
	{
		.name = "Kernel code",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_MEM
	},
	{
		.name = "Kernel data",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_MEM
	}
};

#define kernel_code mem_res[0]
#define kernel_data mem_res[1]

void __init early_print(const char *str, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, str);
	vsnprintf(buf, sizeof(buf), str, ap);
	va_end(ap);

	printk("%s", buf);
}

void __init smp_setup_processor_id(void)
{
	set_my_cpu_offset(0);
}

bool arch_match_cpu_phys_id(int cpu, u64 phys_id)
{
	return phys_id == cpu_logical_map(cpu);
}

struct mpidr_hash mpidr_hash;
#ifdef CONFIG_SMP
static void __init smp_build_mpidr_hash(void)
{
	u32 i, affinity, fs[4], bits[4], ls;
	u64 mask = 0;
	for_each_possible_cpu(i)
		mask |= (cpu_logical_map(i) ^ cpu_logical_map(0));
	pr_debug("mask of set bits %#llx\n", mask);
	for (i = 0; i < 4; i++) {
		affinity = MPIDR_AFFINITY_LEVEL(mask, i);
		ls = fls(affinity);
		fs[i] = affinity ? ffs(affinity) - 1 : 0;
		bits[i] = ls - fs[i];
	}
	mpidr_hash.shift_aff[0] = MPIDR_LEVEL_SHIFT(0) + fs[0];
	mpidr_hash.shift_aff[1] = MPIDR_LEVEL_SHIFT(1) + fs[1] - bits[0];
	mpidr_hash.shift_aff[2] = MPIDR_LEVEL_SHIFT(2) + fs[2] -
						(bits[1] + bits[0]);
	mpidr_hash.shift_aff[3] = MPIDR_LEVEL_SHIFT(3) +
				  fs[3] - (bits[2] + bits[1] + bits[0]);
	mpidr_hash.mask = mask;
	mpidr_hash.bits = bits[3] + bits[2] + bits[1] + bits[0];
	pr_debug("MPIDR hash: aff0[%u] aff1[%u] aff2[%u] aff3[%u] mask[%#llx] bits[%u]\n",
		mpidr_hash.shift_aff[0],
		mpidr_hash.shift_aff[1],
		mpidr_hash.shift_aff[2],
		mpidr_hash.shift_aff[3],
		mpidr_hash.mask,
		mpidr_hash.bits);
	if (mpidr_hash_size() > 4 * num_possible_cpus())
		pr_warn("Large number of MPIDR hash buckets detected\n");
	__flush_dcache_area(&mpidr_hash, sizeof(struct mpidr_hash));
}
#endif

static void __init setup_processor(void)
{
	struct cpu_info *cpu_info;
	u64 features, block;
	u32 cwg;
	int cls;

	cpu_info = lookup_processor_type(read_cpuid_id());
	if (!cpu_info) {
		printk("CPU configuration botched (ID %08x), unable to continue.\n",
		       read_cpuid_id());
		while (1);
	}

	cpu_name = cpu_info->cpu_name;

	printk("CPU: %s [%08x] revision %d\n",
	       cpu_name, read_cpuid_id(), read_cpuid_id() & 15);

	sprintf(init_utsname()->machine, ELF_PLATFORM);
	elf_hwcap = 0;

	cpuinfo_store_boot_cpu();

	cwg = cache_type_cwg();
	cls = cache_line_size();
	if (!cwg)
		pr_warn("No Cache Writeback Granule information, assuming cache line size %d\n",
			cls);
	if (L1_CACHE_BYTES < cls)
		pr_warn("L1_CACHE_BYTES smaller than the Cache Writeback Granule (%d < %d)\n",
			L1_CACHE_BYTES, cls);

	features = read_cpuid(ID_AA64ISAR0_EL1);
	block = (features >> 4) & 0xf;
	if (!(block & 0x8)) {
		switch (block) {
		default:
		case 2:
			elf_hwcap |= HWCAP_PMULL;
		case 1:
			elf_hwcap |= HWCAP_AES;
		case 0:
			break;
		}
	}

	block = (features >> 8) & 0xf;
	if (block && !(block & 0x8))
		elf_hwcap |= HWCAP_SHA1;

	block = (features >> 12) & 0xf;
	if (block && !(block & 0x8))
		elf_hwcap |= HWCAP_SHA2;

	block = (features >> 16) & 0xf;
	if (block && !(block & 0x8))
		elf_hwcap |= HWCAP_CRC32;

#ifdef CONFIG_COMPAT
	features = read_cpuid(ID_ISAR5_EL1);
	block = (features >> 4) & 0xf;
	if (!(block & 0x8)) {
		switch (block) {
		default:
		case 2:
			compat_elf_hwcap2 |= COMPAT_HWCAP2_PMULL;
		case 1:
			compat_elf_hwcap2 |= COMPAT_HWCAP2_AES;
		case 0:
			break;
		}
	}

	block = (features >> 8) & 0xf;
	if (block && !(block & 0x8))
		compat_elf_hwcap2 |= COMPAT_HWCAP2_SHA1;

	block = (features >> 12) & 0xf;
	if (block && !(block & 0x8))
		compat_elf_hwcap2 |= COMPAT_HWCAP2_SHA2;

	block = (features >> 16) & 0xf;
	if (block && !(block & 0x8))
		compat_elf_hwcap2 |= COMPAT_HWCAP2_CRC32;
#endif
}

static void __init setup_machine_fdt(phys_addr_t dt_phys)
{
	if (!dt_phys || !early_init_dt_scan(phys_to_virt(dt_phys))) {
		early_print("\n"
			"Error: invalid device tree blob at physical address 0x%p (virtual address 0x%p)\n"
			"The dtb must be 8-byte aligned and passed in the first 512MB of memory\n"
			"\nPlease check your bootloader.\n",
			dt_phys, phys_to_virt(dt_phys));

		while (true)
			cpu_relax();
	}

	machine_name = of_flat_dt_get_machine_name();
	if (machine_name) {
		dump_stack_set_arch_desc("%s (DT)", machine_name);
		pr_info("Machine: %s\n", machine_name);
	}
}

static int __init early_mem(char *p)
{
	phys_addr_t limit;

	if (!p)
		return 1;

	limit = memparse(p, &p) & PAGE_MASK;
	pr_notice("Memory limited to %lldMB\n", limit >> 20);

	memblock_enforce_memory_limit(limit);

	return 0;
}
early_param("mem", early_mem);

static void __init request_standard_resources(void)
{
	struct memblock_region *region;
	struct resource *res;

	kernel_code.start   = virt_to_phys(_text);
	kernel_code.end     = virt_to_phys(_etext - 1);
	kernel_data.start   = virt_to_phys(_sdata);
	kernel_data.end     = virt_to_phys(_end - 1);

	for_each_memblock(memory, region) {
		res = alloc_bootmem_low(sizeof(*res));
		res->name  = "System RAM";
		res->start = __pfn_to_phys(memblock_region_memory_base_pfn(region));
		res->end = __pfn_to_phys(memblock_region_memory_end_pfn(region)) - 1;
		res->flags = IORESOURCE_MEM | IORESOURCE_BUSY;

		request_resource(&iomem_resource, res);

		if (kernel_code.start >= res->start &&
		    kernel_code.end <= res->end)
			request_resource(res, &kernel_code);
		if (kernel_data.start >= res->start &&
		    kernel_data.end <= res->end)
			request_resource(res, &kernel_data);
	}
}

#ifdef CONFIG_BLK_DEV_INITRD
static void __init relocate_initrd(void)
{
	phys_addr_t orig_start = __virt_to_phys(initrd_start);
	phys_addr_t orig_end = __virt_to_phys(initrd_end);
	phys_addr_t ram_end = memblock_end_of_DRAM();
	phys_addr_t new_start;
	unsigned long size, to_free = 0;
	void *dest;

	if (orig_end <= ram_end)
		return;

	if (orig_start < ram_end)
		to_free = ram_end - orig_start;

	size = orig_end - orig_start;
	if (!size)
		return;

	
	new_start = memblock_find_in_range(0, PFN_PHYS(max_pfn),
					   size, PAGE_SIZE);
	if (!new_start)
		panic("Cannot relocate initrd of size %ld\n", size);
	memblock_reserve(new_start, size);

	initrd_start = __phys_to_virt(new_start);
	initrd_end   = initrd_start + size;

	pr_info("Moving initrd from [%llx-%llx] to [%llx-%llx]\n",
		orig_start, orig_start + size - 1,
		new_start, new_start + size - 1);

	dest = (void *)initrd_start;

	if (to_free) {
		memcpy(dest, (void *)__phys_to_virt(orig_start), to_free);
		dest += to_free;
	}

	copy_from_early_mem(dest, orig_start + to_free, size - to_free);

	if (to_free) {
		pr_info("Freeing original RAMDISK from [%llx-%llx]\n",
			orig_start, orig_start + to_free - 1);
		memblock_free(orig_start, to_free);
	}
}
#else
static inline void __init relocate_initrd(void)
{
}
#endif

u64 __cpu_logical_map[NR_CPUS] = { [0 ... NR_CPUS-1] = INVALID_HWID };

void __init __weak init_random_pool(void) { }

void __init setup_arch(char **cmdline_p)
{
	setup_processor();

	setup_machine_fdt(__fdt_pointer);

	init_mm.start_code = (unsigned long) _text;
	init_mm.end_code   = (unsigned long) _etext;
	init_mm.end_data   = (unsigned long) _edata;
	init_mm.brk	   = (unsigned long) _end;

	*cmdline_p = boot_command_line;

	early_fixmap_init();
	early_ioremap_init();

	parse_early_param();

	local_async_enable();

	efi_init();
	arm64_memblock_init();

	paging_init();
	relocate_initrd();

	kasan_init();

	request_standard_resources();

	efi_virtmap_init();
	early_ioremap_reset();

	unflatten_device_tree();

	psci_init();

	cpu_logical_map(0) = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;
	cpu_read_bootcpu_ops();
#ifdef CONFIG_SMP
	smp_init_cpus();
	smp_build_mpidr_hash();
#endif

#ifdef CONFIG_VT
#if defined(CONFIG_VGA_CONSOLE)
	conswitchp = &vga_con;
#elif defined(CONFIG_DUMMY_CONSOLE)
	conswitchp = &dummy_con;
#endif
#endif
	init_random_pool();
}

static int __init arm64_device_init(void)
{
	of_platform_populate(NULL, of_default_bus_match_table, NULL, NULL);
	return 0;
}
arch_initcall_sync(arm64_device_init);

static int __init topology_init(void)
{
	int i;

	for_each_possible_cpu(i) {
		struct cpu *cpu = &per_cpu(cpu_data.cpu, i);
		cpu->hotpluggable = 1;
		register_cpu(cpu, i);
	}

	return 0;
}
postcore_initcall(topology_init);

static const char *hwcap_str[] = {
	"fp",
	"asimd",
	"evtstrm",
	"aes",
	"pmull",
	"sha1",
	"sha2",
	"crc32",
	NULL
};

#ifdef CONFIG_COMPAT
static const char *compat_hwcap_str[] = {
	"swp",
	"half",
	"thumb",
	"26bit",
	"fastmult",
	"fpa",
	"vfp",
	"edsp",
	"java",
	"iwmmxt",
	"crunch",
	"thumbee",
	"neon",
	"vfpv3",
	"vfpv3d16",
	"tls",
	"vfpv4",
	"idiva",
	"idivt",
	"vfpd32",
	"lpae",
	"evtstrm",
	NULL
};

static const char *compat_hwcap2_str[] = {
	"aes",
	"pmull",
	"sha1",
	"sha2",
	"crc32",
	NULL
};
#endif 

static u32 cx_fuse_data = 0x0;
static u32 mx_fuse_data = 0x0;

static const u32 vddcx_pvs_retention_data[8] =
{
   600000,
   550000,
   500000,
   450000,
   400000,
   400000, 
   400000, 
   600000
};

static const u32 vddmx_pvs_retention_data[8] =
{
   700000,
   650000,
   580000,
   550000,
   490000,
   490000,
   490000,
   490000
};

static int read_cx_fuse_setting(void){
	if(cx_fuse_data != 0x0)
		
		return ((cx_fuse_data & (0x7 << 29)) >> 29);
	else
		return -ENOMEM;
}

static int read_mx_fuse_setting(void){
	if(mx_fuse_data != 0x0)
		
		return ((mx_fuse_data & (0x7 << 2)) >> 2);
	else
		return -ENOMEM;
}

static u32 Get_min_cx(void) {
	u32 lookup_val = 0;
	int mapping_data;
	mapping_data = read_cx_fuse_setting();
	if(mapping_data >= 0)
		lookup_val = vddcx_pvs_retention_data[mapping_data];
	return lookup_val;
}

static u32 Get_min_mx(void) {
	u32 lookup_val = 0;
	int mapping_data;
	mapping_data = read_mx_fuse_setting();
	if(mapping_data >= 0)
		lookup_val = vddmx_pvs_retention_data[mapping_data];
	return lookup_val;
}

extern u64* htc_target_quot[2];
extern int htc_target_quot_len;
static int c_show(struct seq_file *m, void *v)
{
	int i, j, size;

	seq_printf(m, "Processor\t: %s rev %d (%s)\n",
		cpu_name, read_cpuid_id() & 15, ELF_PLATFORM);
	for_each_present_cpu(i) {
		struct cpuinfo_arm64 *cpuinfo = &per_cpu(cpu_data, i);
		u32 midr = cpuinfo->reg_midr;

#ifdef CONFIG_SMP
		seq_printf(m, "processor\t: %d\n", i);
#endif
		seq_printf(m, "min_vddcx\t: %d\n", Get_min_cx());
		seq_printf(m, "min_vddmx\t: %d\n", Get_min_mx());

		seq_printf(m, "BogoMIPS\t: %lu.%02lu\n",
			   loops_per_jiffy / (500000UL/HZ),
			   loops_per_jiffy / (5000UL/HZ) % 100);

		seq_puts(m, "Features\t:");
		if (personality(current->personality) == PER_LINUX32) {
#ifdef CONFIG_COMPAT
			for (j = 0; compat_hwcap_str[j]; j++)
				if (compat_elf_hwcap & (1 << j))
					seq_printf(m, " %s", compat_hwcap_str[j]);

			for (j = 0; compat_hwcap2_str[j]; j++)
				if (compat_elf_hwcap2 & (1 << j))
					seq_printf(m, " %s", compat_hwcap2_str[j]);
#endif 
		} else {
			for (j = 0; hwcap_str[j]; j++)
				if (elf_hwcap & (1 << j))
					seq_printf(m, " %s", hwcap_str[j]);
		}
		seq_puts(m, "\n");

		seq_printf(m, "CPU implementer\t: 0x%02x\n",
			   MIDR_IMPLEMENTOR(midr));
		seq_printf(m, "CPU architecture: 8\n");
		seq_printf(m, "CPU variant\t: 0x%x\n", MIDR_VARIANT(midr));
		seq_printf(m, "CPU part\t: 0x%03x\n", MIDR_PARTNUM(midr));
		seq_printf(m, "CPU revision\t: %d\n", MIDR_REVISION(midr));

		if (!arch_read_hardware_id)
			seq_printf(m, "Hardware\t: %s\n", machine_name);
		else
			seq_printf(m, "Hardware\t: %s\n", arch_read_hardware_id());

		seq_puts(m, "\n");
	}
	size = sizeof(htc_target_quot)/sizeof(u64);
	seq_printf(m, "CPU param\t: ");
	for (i = 0; i < size; i++) {
		if(htc_target_quot[i]) {
			for(j = 0; j < htc_target_quot_len; j++)
				seq_printf(m, "%lld ", htc_target_quot[i][j]);
		}
	}
	seq_printf(m, "\n: ");

	if (!arch_read_hardware_id)
		seq_printf(m, "Hardware\t: %s\n", machine_name);
	else
		seq_printf(m, "Hardware\t: %s\n", arch_read_hardware_id());

	return 0;
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	return *pos < 1 ? (void *)1 : NULL;
}

static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}

static void c_stop(struct seq_file *m, void *v)
{
}

const struct seq_operations cpuinfo_op = {
	.start	= c_start,
	.next	= c_next,
	.stop	= c_stop,
	.show	= c_show
};

void arch_setup_pdev_archdata(struct platform_device *pdev)
{
	pdev->archdata.dma_mask = DMA_BIT_MASK(32);
	pdev->dev.dma_mask = &pdev->archdata.dma_mask;
}

static int msm8996_read_cx_fuse(void){
	void __iomem *addr;
	struct device_node *dn = of_find_compatible_node(NULL,
						NULL, "qcom,cpucx-8996");
	if (dn && (cx_fuse_data == 0x0)) {
		addr = of_iomap(dn, 0);
		if (!addr)
			return -ENOMEM;
		cx_fuse_data = readl_relaxed(addr);
		iounmap(addr);
	}
	else {
		return -ENOMEM;
	}
	return 0;
}
arch_initcall_sync(msm8996_read_cx_fuse);

static int msm8996_read_mx_fuse(void){
	void __iomem *addr;
	struct device_node *dn = of_find_compatible_node(NULL,
						NULL, "qcom,cpumx-8996");
	if (dn && (mx_fuse_data == 0x0)) {
		addr = of_iomap(dn, 0);
		if (!addr)
			return -ENOMEM;
		mx_fuse_data = readl_relaxed(addr);
		iounmap(addr);
	}
	else {
		return -ENOMEM;
	}
	return 0;
}

arch_initcall_sync(msm8996_read_mx_fuse);