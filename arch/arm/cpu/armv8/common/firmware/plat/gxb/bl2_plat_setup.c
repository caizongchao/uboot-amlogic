/*
 * Copyright (c) 2013-2014, ARM Limited and Contributors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of ARM nor the names of its contributors may be used
 * to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <arch_helpers.h>
#include <assert.h>
#include <bl_common.h>
#include <console.h>
#include <debug.h>
#include <platform.h>
#include <platform_def.h>
#include <string.h>
#include "plat_def.h"
#include "plat_private.h"
#include "scp_bootloader.h"
#include <xlat_tables.h>
#include <io.h>
#include "storage.h"

/*******************************************************************************
 * Declarations of linker defined symbols which will help us find the layout
 * of trusted RAM
 ******************************************************************************/
extern unsigned long __RO_START__;
extern unsigned long __RO_END__;

extern unsigned long __COHERENT_RAM_START__;
extern unsigned long __COHERENT_RAM_END__;

/*
 * The next 2 constants identify the extents of the code & RO data region.
 * These addresses are used by the MMU setup code and therefore they must be
 * page-aligned.  It is the responsibility of the linker script to ensure that
 * __RO_START__ and __RO_END__ linker symbols refer to page-aligned addresses.
 */
#define BL2_RO_BASE (unsigned long)(&__RO_START__)
#define BL2_RO_LIMIT (unsigned long)(&__RO_END__)

/*
 * The next 2 constants identify the extents of the coherent memory region.
 * These addresses are used by the MMU setup code and therefore they must be
 * page-aligned.  It is the responsibility of the linker script to ensure that
 * __COHERENT_RAM_START__ and __COHERENT_RAM_END__ linker symbols refer to
 * page-aligned addresses.
 */
#define BL2_COHERENT_RAM_BASE (unsigned long)(&__COHERENT_RAM_START__)
#define BL2_COHERENT_RAM_LIMIT (unsigned long)(&__COHERENT_RAM_END__)

/* Data structure which holds the extents of the trusted RAM for BL2 */
static meminfo_t bl2_tzram_layout
__attribute__ ((aligned(PLATFORM_CACHE_LINE_SIZE),
		section("tzfw_coherent_mem")));

/*******************************************************************************
 * Structure which holds the arguments which need to be passed to BL3-1
 ******************************************************************************/
static bl2_to_bl31_params_mem_t bl31_params_mem;

meminfo_t *bl2_plat_sec_mem_layout(void)
{
	return &bl2_tzram_layout;
}

/*******************************************************************************
 * This function assigns a pointer to the memory that the platform has kept
 * aside to pass platform specific and trusted firmware related information
 * to BL31. This memory is allocated by allocating memory to
 * bl2_to_bl31_params_mem_t structure which is a superset of all the
 * structure whose information is passed to BL31
 * NOTE: This function should be called only once and should be done
 * before generating params to BL31
 ******************************************************************************/
bl31_params_t *bl2_plat_get_bl31_params(void)
{
	bl31_params_t *bl2_to_bl31_params;

	/*
	 * Initialise the memory for all the arguments that needs to
	 * be passed to BL3-1
	 */
	memset(&bl31_params_mem, 0, sizeof(bl2_to_bl31_params_mem_t));

	/* Assign memory for TF related information */
	bl2_to_bl31_params = &bl31_params_mem.bl31_params;
	SET_PARAM_HEAD(bl2_to_bl31_params, PARAM_BL31, VERSION_1, 0);

	/* Fill BL3-1 related information */
	bl2_to_bl31_params->bl31_image_info = &bl31_params_mem.bl31_image_info;
	SET_PARAM_HEAD(bl2_to_bl31_params->bl31_image_info, PARAM_IMAGE_BINARY,
		VERSION_1, 0);

	/* Fill BL3-2 related information if it exists */
#if BL32_BASE
	bl2_to_bl31_params->bl32_ep_info = &bl31_params_mem.bl32_ep_info;
	SET_PARAM_HEAD(bl2_to_bl31_params->bl32_ep_info, PARAM_EP,
		VERSION_1, 0);
	bl2_to_bl31_params->bl32_image_info = &bl31_params_mem.bl32_image_info;
	SET_PARAM_HEAD(bl2_to_bl31_params->bl32_image_info, PARAM_IMAGE_BINARY,
		VERSION_1, 0);
#endif

	/* Fill BL3-3 related information */
	bl2_to_bl31_params->bl33_ep_info = &bl31_params_mem.bl33_ep_info;
	SET_PARAM_HEAD(bl2_to_bl31_params->bl33_ep_info,
		PARAM_EP, VERSION_1, 0);
	/* UEFI expects to receive the primary CPU MPID (through x0) */
	bl2_to_bl31_params->bl33_ep_info->args.arg0 = PRIMARY_CPU;

	bl2_to_bl31_params->bl33_image_info = &bl31_params_mem.bl33_image_info;
	SET_PARAM_HEAD(bl2_to_bl31_params->bl33_image_info, PARAM_IMAGE_BINARY,
		VERSION_1, 0);

	return bl2_to_bl31_params;
}

/*******************************************************************************
 * This function returns a pointer to the shared memory that the platform
 * has kept to point to entry point information of BL31 to BL2
 ******************************************************************************/
struct entry_point_info *bl2_plat_get_bl31_ep_info(void)
{
#if DEBUG
	bl31_params_mem.bl31_ep_info.args.arg1 = PLAT_BL31_PLAT_PARAM_VAL;
#endif

	return &bl31_params_mem.bl31_ep_info;
}

/*******************************************************************************
 * BL1 has passed the extents of the trusted RAM that should be visible to BL2
 * in x0. This memory layout is sitting at the base of the free trusted RAM.
 * Copy it to a safe loaction before its reclaimed by later BL2 functionality.
 ******************************************************************************/
void bl2_early_platform_setup(meminfo_t *mem_layout)
{
	//console_init(52);   //24M

	bl2_tzram_layout.total_base = TZRAM_BASE;
	bl2_tzram_layout.total_size = TZRAM_SIZE;
	bl2_tzram_layout.free_base = TZRAM_BL2_FREE_BASE;
	bl2_tzram_layout.free_size = TZRAM_BL2_FREE_SIZE;
	bl2_tzram_layout.attr = (unsigned long)0x0E939E2E8CF8EFFD;
	bl2_tzram_layout.next = 0;
}

/*******************************************************************************
 * Perform platform specific setup, i.e. initialize the IO layer, load BL3-0
 * image and initialise the memory location to use for passing arguments to
 * BL3-1.
 ******************************************************************************/
void bl2_platform_setup(void)
{
#ifdef BL2_ENABLE_MMU
	/* setup mmu */
	configure_mmu_el1(bl2_tzram_layout.total_base,
			  bl2_tzram_layout.total_size,
			  BL2_RO_BASE,
			  BL2_RO_LIMIT,
			  BL2_COHERENT_RAM_BASE,
			  BL2_COHERENT_RAM_LIMIT);
#endif

  /* storage init */
  storage_init();
}

/* Flush the TF params and the TF plat params */
void bl2_plat_flush_bl31_params(void)
{
	flush_dcache_range((unsigned long)&bl31_params_mem,
			sizeof(bl2_to_bl31_params_mem_t));
}

/*******************************************************************************
 * Perform the very early platform specific architectural setup here. At the
 * moment this is only intializes the mmu in a quick and dirty way.
 ******************************************************************************/
void bl2_plat_arch_setup(void)
{
	return;
}

/*******************************************************************************
 * Before calling this function BL31 is loaded in memory and its entrypoint
 * is set by load_image. This is a placeholder for the platform to change
 * the entrypoint of BL31 and set SPSR and security state.
 * On ${PLAT} we are only setting the security state, entrypoint
 ******************************************************************************/
void bl2_plat_set_bl31_ep_info(image_info_t *bl31_image_info,
			       entry_point_info_t *bl31_ep_info)
{
	SET_SECURITY_STATE(bl31_ep_info->h.attr, SECURE);
	bl31_ep_info->spsr = SPSR_64(MODE_EL3, MODE_SP_ELX,
				       DISABLE_ALL_EXCEPTIONS);
}


/*******************************************************************************
 * Before calling this function BL32 is loaded in memory and its entrypoint
 * is set by load_image. This is a placeholder for the platform to change
 * the entrypoint of BL32 and set SPSR and security state.
 * On ${PLAT} we are only setting the security state, entrypoint
 ******************************************************************************/
void bl2_plat_set_bl32_ep_info(image_info_t *bl32_image_info,
			       entry_point_info_t *bl32_ep_info)
{
	SET_SECURITY_STATE(bl32_ep_info->h.attr, SECURE);
	/*
	* The Secure Payload Dispatcher service is responsible for
	* setting the SPSR prior to entry into the BL32 image.
	*/
	bl32_ep_info->spsr = 0;
}

/*******************************************************************************
 * Before calling this function BL33 is loaded in memory and its entrypoint
 * is set by load_image. This is a placeholder for the platform to change
 * the entrypoint of BL33 and set SPSR and security state.
 * On ${PLAT} we are only setting the security state, entrypoint
 ******************************************************************************/
void bl2_plat_set_bl33_ep_info(image_info_t *image,
				       entry_point_info_t *bl33_ep_info)
{
	unsigned long el_status;
	unsigned int mode;

	/* Figure out what mode we enter the non-secure world in */
	el_status = read_id_aa64pfr0_el1() >> ID_AA64PFR0_EL2_SHIFT;
	el_status &= ID_AA64PFR0_ELX_MASK;

	if (el_status)
		mode = MODE_EL2;
	else
		mode = MODE_EL1;

	/*
	 * TODO: Consider the possibility of specifying the SPSR in
	 * the FIP ToC and allowing the platform to have a say as
	 * well.
	 */
	bl33_ep_info->spsr = SPSR_64(mode, MODE_SP_ELX,
				       DISABLE_ALL_EXCEPTIONS);
	SET_SECURITY_STATE(bl33_ep_info->h.attr, NON_SECURE);
}

/*******************************************************************************
 * Populate the extents of memory available for loading BL3-2
 ******************************************************************************/
#ifdef BL32_BASE
void bl2_plat_get_bl32_meminfo(meminfo_t *bl32_meminfo)
{
	/*
	 * Populate the extents of memory available for loading BL3-2.
	 */
	bl32_meminfo->total_base = BL32_BASE;
	bl32_meminfo->free_base = BL32_BASE;
	bl32_meminfo->total_size =
		       (TSP_SEC_MEM_BASE + TSP_SEC_MEM_SIZE) - BL32_BASE;
	bl32_meminfo->free_size =
		       (TSP_SEC_MEM_BASE + TSP_SEC_MEM_SIZE) - BL32_BASE;
	bl32_meminfo->attr = BOT_LOAD;
	bl32_meminfo->next = 0;
}
#endif

/*******************************************************************************
 * Populate the extents of memory available for loading BL3-3
 ******************************************************************************/
void bl2_plat_get_bl33_meminfo(meminfo_t *bl33_meminfo)
{
	bl33_meminfo->total_base = DRAM_BASE;
	bl33_meminfo->total_size = DRAM_SIZE;
	bl33_meminfo->free_base = DRAM_BASE;
	bl33_meminfo->free_size = DRAM_SIZE;
	bl33_meminfo->attr = 0;
	bl33_meminfo->attr = 0;
}