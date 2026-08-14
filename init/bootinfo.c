/*
 * init/bootinfo.c - OpenSBI-style boot banner
 *
 * Banner blocks live in the modules that own their data; this file holds
 * only the banner-level pieces: the version/logo line, and bootinfo_mm()
 * which sequences the four MM row printers into the memory block.
 */

#include <nuvix/bootinfo.h>

#define NUVIX_VERSION "0.1"

static const char logo[] = " ███╗   ██╗██╗   ██╗██╗   ██╗██╗██╗  ██╗\n"
			   " ████╗  ██║██║   ██║██║   ██║██║╚██╗██╔╝\n"
			   " ██╔██╗ ██║██║   ██║██║   ██║██║ ╚███╔╝\n"
			   " ██║╚██╗██║██║   ██║╚██╗ ██╔╝██║ ██╔██╗\n"
			   " ██║ ╚████║╚██████╔╝ ╚████╔╝ ██║██╔╝ ██╗\n"
			   " ╚═╝  ╚═══╝ ╚═════╝   ╚═══╝  ╚═╝╚═╝  ╚═╝\n";

void bootinfo_logo(void)
{
	pr_info("\n");
	pr_info("nuvix v%s\n", NUVIX_VERSION);
	pr_info("%s", logo);
	pr_info("\n");
}

BOOTINFO_BLOCK(mm, void,
	bootinfo_buddy();
	bootinfo_slab();
	bootinfo_vmalloc();
)
