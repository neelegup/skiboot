/* Copyright 2013-2014 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <skiboot.h>
#include <xscom.h>
#include <io.h>
#include <cpu.h>
#include <chip.h>
#include <mem_region.h>
#include <fsp.h>
#include <timebase.h>
#include <hostservices.h>
#include <errorlog.h>

/* OCC Communication Area for PStates */

#define P8_HOMER_SAPPHIRE_DATA_OFFSET	0x1F8000

#define MAX_PSTATES 256

struct occ_pstate_entry {
	s8 id;
	u8 flags;
	u8 vdd;
	u8 vcs;
	u32 freq_khz;
};

struct occ_pstate_table {
	u8 valid;
	u8 version;
	u8 throttle;
	s8 pstate_min;
	s8 pstate_nom;
	s8 pstate_max;
	u8 spare1;
	u8 spare2;
	u64 reserved;
	struct occ_pstate_entry pstates[MAX_PSTATES];
};

DEFINE_LOG_ENTRY(OPAL_RC_OCC_LOAD, OPAL_PLATFORM_ERR_EVT, OPAL_OCC,
		OPAL_CEC_HARDWARE, OPAL_PREDICTIVE_ERR_GENERAL,
		OPAL_NA, NULL);

DEFINE_LOG_ENTRY(OPAL_RC_OCC_RESET, OPAL_PLATFORM_ERR_EVT, OPAL_OCC,
		OPAL_CEC_HARDWARE, OPAL_PREDICTIVE_ERR_GENERAL,
		OPAL_NA, NULL);

DEFINE_LOG_ENTRY(OPAL_RC_OCC_PSTATE_INIT, OPAL_PLATFORM_ERR_EVT, OPAL_OCC,
		OPAL_CEC_HARDWARE, OPAL_INFO,
		OPAL_NA, NULL);

DEFINE_LOG_ENTRY(OPAL_RC_OCC_TIMEOUT, OPAL_PLATFORM_ERR_EVT, OPAL_OCC,
		 OPAL_CEC_HARDWARE, OPAL_UNRECOVERABLE_ERR_GENERAL,
		 OPAL_NA, NULL);

/* Check each chip's HOMER/Sapphire area for PState valid bit */
static bool wait_for_all_occ_init(void)
{
	struct proc_chip *chip;
	uint64_t occ_data_area;
	struct occ_pstate_table *occ_data;
	int tries;
	uint64_t start_time, end_time;
	uint32_t timeout = 0;

	if (platform.occ_timeout)
		timeout = platform.occ_timeout();

	start_time = mftb();
	for_each_chip(chip) {
		/* Check for valid homer address */
		if (!chip->homer_base) {
			prerror("OCC: Chip: %x homer_base is not valid\n",
				chip->id);
			return false;
		}
		/* Get PState table address */
		occ_data_area = chip->homer_base + P8_HOMER_SAPPHIRE_DATA_OFFSET;
		occ_data = (struct occ_pstate_table *)occ_data_area;

		/*
		 * Checking for occ_data->valid == 1 is ok because we clear all
		 * homer_base+size before passing memory to host services.
		 * This ensures occ_data->valid == 0 before OCC load
		 */
		tries = timeout * 10;
		while((occ_data->valid != 1) && tries--) {
			time_wait_ms(100);
		}
		if (occ_data->valid != 1) {
			prerror("OCC: Chip: %x PState table is not valid\n",
				chip->id);
			return false;
		}
		prlog(PR_DEBUG, "OCC: Chip %02x Data (%016llx) = %016llx\n",
		      chip->id, occ_data_area,
		      *(uint64_t *)occ_data_area);
	}
	end_time = mftb();
	prlog(PR_NOTICE, "OCC: All Chip Rdy after %lld ms\n",
	      (end_time - start_time) / 512 / 1000);
	return true;
}

/* Add device tree properties to describe pstates states */
/* Retrun nominal pstate to set in each core */
static bool add_cpu_pstate_properties(s8 *pstate_nom)
{
	struct proc_chip *chip;
	uint64_t occ_data_area;
	struct occ_pstate_table *occ_data;
	struct dt_node *power_mgt;
	u8 nr_pstates;
	/* Arrays for device tree */
	u32 *dt_id, *dt_freq;
	u8 *dt_vdd, *dt_vcs;
	bool rc;
	int i;

	prlog(PR_DEBUG, "OCC: CPU pstate state device tree init\n");

	/* Find first chip and core */
	chip = next_chip(NULL);

	/* Extract PState information from OCC */

	/* Dump state table */
	occ_data_area = chip->homer_base + P8_HOMER_SAPPHIRE_DATA_OFFSET;

	prlog(PR_DEBUG, "OCC: Data (%16llx) = %16llx %16llx\n",
	      occ_data_area,
	      *(uint64_t *)occ_data_area,
	      *(uint64_t *)(occ_data_area+8));
	
	occ_data = (struct occ_pstate_table *)occ_data_area;

	if (!occ_data->valid) {
		prerror("OCC: PState table is not valid\n");
		return false;
	}

	nr_pstates = occ_data->pstate_max - occ_data->pstate_min + 1;
	prlog(PR_DEBUG, "OCC: Min %d Nom %d Max %d Nr States %d\n", 
	      occ_data->pstate_min, occ_data->pstate_nom,
	      occ_data->pstate_max, nr_pstates);

	if (nr_pstates <= 1 || nr_pstates > 128) {
		prerror("OCC: OCC range is not valid\n");
		return false;
	}

	power_mgt = dt_find_by_path(dt_root, "/ibm,opal/power-mgt");
	if (!power_mgt) {
		prerror("OCC: dt node /ibm,opal/power-mgt not found\n");
		return false;
	}

	rc = false;

	/* Setup arrays for device-tree */
	/* Allocate memory */
	dt_id = (u32 *) malloc(MAX_PSTATES * sizeof(u32));
	if (!dt_id) {
		printf("OCC: dt_id array alloc failure\n");
		goto out;
	}

	dt_freq = (u32 *) malloc(MAX_PSTATES * sizeof(u32));
	if (!dt_freq) {
		printf("OCC: dt_freq array alloc failure\n");
		goto out_free_id;
	}

	dt_vdd = (u8 *) malloc(MAX_PSTATES * sizeof(u8));
	if (!dt_vdd) {
		printf("OCC: dt_vdd array alloc failure\n");
		goto out_free_freq;
	}

	dt_vcs = (u8 *) malloc(MAX_PSTATES * sizeof(u8));
	if (!dt_vcs) {
		printf("OCC: dt_vcs array alloc failure\n");
		goto out_free_vdd;
	}

	for( i=0; i < nr_pstates; i++) {
		dt_id[i] = occ_data->pstates[i].id;
		dt_freq[i] = occ_data->pstates[i].freq_khz/1000;
		dt_vdd[i] = occ_data->pstates[i].vdd;
		dt_vcs[i] = occ_data->pstates[i].vcs;
	}

	/* Add the device-tree entries */
	dt_add_property(power_mgt, "ibm,pstate-ids", dt_id, nr_pstates * 4);
	dt_add_property(power_mgt, "ibm,pstate-frequencies-mhz", dt_freq, nr_pstates * 4);
	dt_add_property(power_mgt, "ibm,pstate-vdds", dt_vdd, nr_pstates);
	dt_add_property(power_mgt, "ibm,pstate-vcss", dt_vcs, nr_pstates);
	dt_add_property_cells(power_mgt, "ibm,pstate-min", occ_data->pstate_min);
	dt_add_property_cells(power_mgt, "ibm,pstate-nominal", occ_data->pstate_nom);
	dt_add_property_cells(power_mgt, "ibm,pstate-max", occ_data->pstate_max);

	/* Return pstate to set for each core */
	*pstate_nom = occ_data->pstate_nom;
	rc = true;

	free(dt_vcs);
out_free_vdd:
	free(dt_vdd);
out_free_id:
	free(dt_id);
out_free_freq:
	free(dt_freq);
out:
	return rc;
}

/*
 * Prepare chip for pstate transitions
 */

static bool cpu_pstates_prepare_core(struct proc_chip *chip, struct cpu_thread *c, s8 pstate_nom)
{
	uint32_t core = pir_to_core_id(c->pir);
	uint64_t tmp, pstate;
	int rc;

	/*
	 * Currently Fastsleep init clears EX_PM_SPR_OVERRIDE_EN.
	 * Need to ensure only relevant bits are inited
	 */

	/* Init PM GP1 for SCOM based PSTATE control to set nominal freq
	 *
	 * Use the OR SCOM to set the required bits in PM_GP1 register
	 * since the OCC might be mainpulating the PM_GP1 register as well.
	 */ 
	rc = xscom_write(chip->id, XSCOM_ADDR_P8_EX_SLAVE(core, EX_PM_SET_GP1),
			 EX_PM_SETUP_GP1_PM_SPR_OVERRIDE_EN);
	if (rc) {
		log_simple_error(&e_info(OPAL_RC_OCC_PSTATE_INIT),
			"OCC: Failed to write PM_GP1 in pstates init\n");
		return false;
	}

	/* Set new pstate to core */
	rc = xscom_read(chip->id, XSCOM_ADDR_P8_EX_SLAVE(core, EX_PM_PPMCR), &tmp);
	tmp = tmp & ~0xFFFF000000000000ULL;
	pstate = ((uint64_t) pstate_nom) & 0xFF;
	tmp = tmp | (pstate << 56) | (pstate << 48);
	rc = xscom_write(chip->id, XSCOM_ADDR_P8_EX_SLAVE(core, EX_PM_PPMCR), tmp);
	if (rc) {
		log_simple_error(&e_info(OPAL_RC_OCC_PSTATE_INIT),
			"OCC: Failed to write PM_GP1 in pstates init\n");
		return false;
	}
	time_wait_ms(1); /* Wait for PState to change */
	/*
	 * Init PM GP1 for SPR based PSTATE control.
	 * Once OCC is active EX_PM_SETUP_GP1_DPLL_FREQ_OVERRIDE_EN will be
	 * cleared by OCC.  Sapphire need not clear.
	 * However wait for DVFS state machine to become idle after min->nominal
	 * transition initiated above.  If not switch over to SPR control could fail.
	 *
	 * Use the AND SCOM to clear the required bits in PM_GP1 register
	 * since the OCC might be mainpulating the PM_GP1 register as well.
	 */
	tmp = ~EX_PM_SETUP_GP1_PM_SPR_OVERRIDE_EN;
	rc = xscom_write(chip->id, XSCOM_ADDR_P8_EX_SLAVE(core, EX_PM_CLEAR_GP1),
			tmp);
	if (rc) {
		log_simple_error(&e_info(OPAL_RC_OCC_PSTATE_INIT),
			"OCC: Failed to write PM_GP1 in pstates init\n");
		return false;
	}

	/* Just debug */
	rc = xscom_read(chip->id, XSCOM_ADDR_P8_EX_SLAVE(core, EX_PM_PPMSR), &tmp);
	prlog(PR_DEBUG, "OCC: Chip %x Core %x PPMSR %016llx\n",
	      chip->id, core, tmp);

	/*
	 * If PMSR is still in transition at this point due to PState change
	 * initiated above, then the switchover to SPR may not work.
	 * ToDo: Check for DVFS state machine idle before change.
	 */

	return true;
}

/* CPU-OCC PState init */
/* Called after OCC init on P8 */
void occ_pstates_init(void)
{
	struct proc_chip *chip;
	struct cpu_thread *c;
	s8 pstate_nom;

	/* OCC is P8 only */
	if (proc_gen != proc_gen_p8)
		return;

	chip = next_chip(NULL);
	if (!chip->homer_base) {
		log_simple_error(&e_info(OPAL_RC_OCC_PSTATE_INIT),
			"OCC: No HOMER detected, assuming no pstates\n");
		return;
	}

	/* Wait for all OCC to boot up */
	if(!wait_for_all_occ_init()) {
		log_simple_error(&e_info(OPAL_RC_OCC_TIMEOUT),
			 "OCC: Initialization on all chips did not complete"
			 "(timed out)\n");
		return;
	}

	/*
	 * Check boundary conditions and add device tree nodes
	 * and return nominal pstate to set for the core
	 */
	if (!add_cpu_pstate_properties(&pstate_nom)) {
		log_simple_error(&e_info(OPAL_RC_OCC_PSTATE_INIT),
			"Skiping core cpufreq init due to OCC error\n");
		return;
	}

	/* Setup host based pstates and set nominal frequency */
	for_each_chip(chip) {
		for_each_available_core_in_chip(c, chip->id) {
			cpu_pstates_prepare_core(chip, c, pstate_nom);
		}
	}
}

static void occ_do_load(u8 scope, u32 dbob_id __unused, u32 seq_id)
{
	struct fsp_msg *rsp, *stat;
	int rc = -ENOMEM;
	int status_word = 0;
	struct proc_chip *chip = next_chip(NULL);
	u8 err = 0;

	/* Check arguments */
	if (scope != 0x01 && scope != 0x02) {
		prerror("OCC: Load message with invalid scope 0x%x\n",
			scope);
		err = 0x22;
	}

	/* First queue up an OK response to the load message itself */
	rsp = fsp_mkmsg(FSP_RSP_LOAD_OCC | err, 0);
	if (rsp)
		rc = fsp_queue_msg(rsp, fsp_freemsg);
	if (rc) {
		log_simple_error(&e_info(OPAL_RC_OCC_LOAD),
			"OCC: Error %d queueing FSP OCC LOAD reply\n", rc);
		return;
	}

	/* If we had an error, return */
	if (err)
		return;

	/* Call HBRT... */
	rc = host_services_occ_load();

	/* Handle fallback to preload */
	if (rc == -ENOENT && chip->homer_base) {
		prlog(PR_INFO, "OCC: Load: Fallback to preloaded image\n");
		rc = 0;
	} else if (!rc) {
		/* Success, start OCC */
		rc = host_services_occ_start();
	}
	if (rc) {
		/* If either of hostservices call fail, send fail to FSP */
		/* Find a chip ID to send failure */
		for_each_chip(chip) {
			if (scope == 0x01 && dbob_id != chip->dbob_id)
				continue;
			status_word = 0xB500 | (chip->pcid & 0xff);
			break;
		}
		log_simple_error(&e_info(OPAL_RC_OCC_LOAD),
			"OCC: Error %d in load/start OCC\n", err);
	}

	/* Send a single response for all chips */
	stat = fsp_mkmsg(FSP_CMD_LOAD_OCC_STAT, 2, status_word, seq_id);
	if (stat)
		rc = fsp_queue_msg(stat, fsp_freemsg);
	if (rc) {
		log_simple_error(&e_info(OPAL_RC_OCC_LOAD),
			"OCC: Error %d queueing FSP OCC LOAD STATUS msg", rc);
	}
}

static void occ_do_reset(u8 scope, u32 dbob_id, u32 seq_id)
{
	struct fsp_msg *rsp, *stat;
	struct proc_chip *chip = next_chip(NULL);
	int rc = -ENOMEM;
	u8 err = 0;

	/* Check arguments */
	if (scope != 0x01 && scope != 0x02) {
		prerror("OCC: Reset message with invalid scope 0x%x\n",
			scope);
		err = 0x22;
	}

	/* First queue up an OK response to the reset message itself */
	rsp = fsp_mkmsg(FSP_RSP_RESET_OCC | err, 0);
	if (rsp)
		rc = fsp_queue_msg(rsp, fsp_freemsg);
	if (rc) {
		log_simple_error(&e_info(OPAL_RC_OCC_RESET),
			"OCC: Error %d queueing FSP OCC RESET reply\n", rc);
		return;
	}

	/* If we had an error, return */
	if (err)
		return;

	/*
	 * Call HBRT to stop OCC and leave it stopped.  FSP will send load/start
	 * request subsequently.  Also after few runtime restarts (currently 3),
	 * FSP will request OCC to left in stopped state.
	 */

	rc = host_services_occ_stop();

	/* Handle fallback to preload */
	if (rc == -ENOENT && chip->homer_base) {
		prlog(PR_INFO, "OCC: Reset: Fallback to preloaded image\n");
		rc = 0;
	}
	if (!rc) {
		/* Send a single success response for all chips */
		stat = fsp_mkmsg(FSP_CMD_RESET_OCC_STAT, 2, 0, seq_id);
		if (stat)
			rc = fsp_queue_msg(stat, fsp_freemsg);
		if (rc) {
			log_simple_error(&e_info(OPAL_RC_OCC_RESET),
				"OCC: Error %d queueing FSP OCC RESET"
					" STATUS message\n", rc);
		}
	} else {

		/*
		 * Then send a matching OCC Reset Status message with an 0xFE
		 * (fail) response code as well to the first matching chip
		 */
		for_each_chip(chip) {
			if (scope == 0x01 && dbob_id != chip->dbob_id)
				continue;
			rc = -ENOMEM;
			stat = fsp_mkmsg(FSP_CMD_RESET_OCC_STAT, 2,
					 0xfe00 | (chip->pcid & 0xff), seq_id);
			if (stat)
				rc = fsp_queue_msg(stat, fsp_freemsg);
			if (rc) {
				log_simple_error(&e_info(OPAL_RC_OCC_RESET),
					"OCC: Error %d queueing FSP OCC RESET"
						" STATUS message\n", rc);
			}
			break;
		}
	}
}

static bool fsp_occ_msg(u32 cmd_sub_mod, struct fsp_msg *msg)
{
	u32 dbob_id, seq_id;
	u8 scope;

	switch (cmd_sub_mod) {
	case FSP_CMD_LOAD_OCC:
		/*
		 * We get the "Load OCC" command at boot. We don't currently
		 * support loading it ourselves (we don't have the procedures,
		 * they will come with Host Services). For now HostBoot will
		 * have loaded a OCC firmware for us, but we still need to
		 * be nice and respond to OCC.
		 */
		scope = msg->data.bytes[3];
		dbob_id = msg->data.words[1];
		seq_id = msg->data.words[2];
		prlog(PR_INFO, "OCC: Got OCC Load message, scope=0x%x"
		      " dbob=0x%x seq=0x%x\n", scope, dbob_id, seq_id);
		occ_do_load(scope, dbob_id, seq_id);
		return true;

	case FSP_CMD_RESET_OCC:
		/*
		 * We shouldn't be getting this one, but if we do, we have
		 * to reply something sensible or the FSP will get upset
		 */
		scope = msg->data.bytes[3];
		dbob_id = msg->data.words[1];
		seq_id = msg->data.words[2];
		prlog(PR_INFO, "OCC: Got OCC Reset message, scope=0x%x"
		      " dbob=0x%x seq=0x%x\n", scope, dbob_id, seq_id);
		occ_do_reset(scope, dbob_id, seq_id);
		return true;
	}
	return false;
}

static struct fsp_client fsp_occ_client = {
	.message = fsp_occ_msg,
};

#define OCB_OCI_OCCMISC		0x6a020
#define OCB_OCI_OCCMISC_AND	0x6a021
#define OCB_OCI_OCCMISC_OR	0x6a022
#define OCB_OCI_OCIMISC_IRQ		PPC_BIT(0)
#define OCB_OCI_OCIMISC_IRQ_TMGT	PPC_BIT(1)
#define OCB_OCI_OCIMISC_IRQ_OPAL_DUMMY	PPC_BIT(15)

void occ_send_dummy_interrupt(void)
{
	/* Mambo chip and P7 don't do this */
	if (is_mambo_chip || proc_gen != proc_gen_p8)
		return;
	xscom_writeme(OCB_OCI_OCCMISC_OR,
		      OCB_OCI_OCIMISC_IRQ |
		      OCB_OCI_OCIMISC_IRQ_OPAL_DUMMY);
}

static void occ_tmgt_interrupt(void)
{
	/* Not currently expected */
	printf("OCC: TMGT interrupt !\n");
}

void occ_interrupt(uint32_t chip_id)
{
	uint64_t ireg;
	int64_t rc;

	/* The OCC interrupt is used to mux up to 15 different sources */
	rc = xscom_read(chip_id, OCB_OCI_OCCMISC, &ireg);
	if (rc) {
		prerror("OCC: Failed to read interrupt status !\n");
		/* Should we mask it in the XIVR ? */
		return;
	}
	prlog(PR_TRACE, "OCC: IRQ received: %04llx\n", ireg >> 48);

	/* Clear the bits */
	xscom_write(chip_id, OCB_OCI_OCCMISC_AND, ~ireg);

	/* Dispatch */
	if (ireg & OCB_OCI_OCIMISC_IRQ_TMGT)
		occ_tmgt_interrupt();
}

void occ_fsp_init(void)
{
	/* OCC is P8 only */
	if (proc_gen != proc_gen_p8)
		return;

	/* If we have an FSP, register for notifications */
	if (fsp_present())
		fsp_register_client(&fsp_occ_client, FSP_MCLASS_OCC);
}


