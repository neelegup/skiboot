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
#include <opal.h>
#include <opal-api.h>
#include <opal-msg.h>
#include <processor.h>
#include <chiptod.h>
#include <lock.h>
#include <xscom.h>
#include <capp.h>
#include <pci.h>

/*
 * HMER register layout:
 * +===+==========+============================+========+===================+
 * |Bit|Name      |Description                 |PowerKVM|Action             |
 * |   |          |                            |HMI     |                   |
 * |   |          |                            |enabled |                   |
 * |   |          |                            |for this|                   |
 * |   |          |                            |bit ?   |                   |
 * +===+==========+============================+========+===================+
 * |0  |malfunctio|A processor core in the     |Yes     |Raise attn from    |
 * |   |n_allert  |system has checkstopped     |        |sapphire resulting |
 * |   |          |(failed recovery) and has   |        |xstop              |
 * |   |          |requested a CP Sparing      |        |                   |
 * |   |          |to occur. This is           |        |                   |
 * |   |          |broadcasted to every        |        |                   |
 * |   |          |processor in the system     |        |                   |
 * |---+----------+----------------------------+--------+-------------------|
 * |1  |Reserved  |reserved                    |n/a     |                   |
 * |---+----------+----------------------------+--------+-------------------|
 * |2  |proc_recv_|Processor recovery occurred |Yes     |Log message and    |
 * |   |done      |error-bit in fir not masked |        |continue working.  |
 * |   |          |(see bit 11)                |        |                   |
 * |---+----------+----------------------------+--------+-------------------|
 * |3  |proc_recv_|Processor went through      |Yes     |Log message and    |
 * |   |error_mask|recovery for an error which |        |continue working.  |
 * |   |ed        |is actually masked for      |        |                   |
 * |   |          |reporting                   |        |                   |
 * |---+----------+----------------------------+--------+-------------------|
 * |4  |          |Timer facility experienced  |Yes     |Raise attn from    |
 * |   |tfac_error|an error.                   |        |sapphire resulting |
 * |   |          |TB, DEC, HDEC, PURR or SPURR|        |xstop              |
 * |   |          |may be corrupted (details in|        |                   |
 * |   |          |TFMR)                       |        |                   |
 * |---+----------+----------------------------+--------+-------------------|
 * |5  |          |TFMR SPR itself is          |Yes     |Raise attn from    |
 * |   |tfmr_parit|corrupted.                  |        |sapphire resulting |
 * |   |y_error   |Entire timing facility may  |        |xstop              |
 * |   |          |be compromised.             |        |                   |
 * |---+----------+----------------------------+--------+-------------------|
 * |6  |ha_overflo| UPS (Uniterrupted Power    |No      |N/A                |
 * |   |w_warning |System) Overflow indication |        |                   |
 * |   |          |indicating that the UPS     |        |                   |
 * |   |          |DirtyAddrTable has          |        |                   |
 * |   |          |reached a limit where it    |        |                   |
 * |   |          |requires PHYP unload support|        |                   |
 * |---+----------+----------------------------+--------+-------------------|
 * |7  |reserved  |reserved                    |n/a     |n/a                |
 * |---+----------+----------------------------+--------+-------------------|
 * |8  |xscom_fail|An XSCOM operation caused by|No      |We handle it by    |
 * |   |          |a cache inhibited load/store|        |manually reading   |
 * |   |          |from this thread failed. A  |        |HMER register.     |
 * |   |          |trap register is            |        |                   |
 * |   |          |available.                  |        |                   |
 * |   |          |                            |        |                   |
 * |---+----------+----------------------------+--------+-------------------|
 * |9  |xscom_done|An XSCOM operation caused by|No      |We handle it by    |
 * |   |          |a cache inhibited load/store|        |manually reading   |
 * |   |          |from this thread completed. |        |HMER register.     |
 * |   |          |If hypervisor               |        |                   |
 * |   |          |intends to use this bit, it |        |                   |
 * |   |          |is responsible for clearing |        |                   |
 * |   |          |it before performing the    |        |                   |
 * |   |          |xscom operation.            |        |                   |
 * |   |          |NOTE: this bit should always|        |                   |
 * |   |          |be masked in HMEER          |        |                   |
 * |---+----------+----------------------------+--------+-------------------|
 * |10 |reserved  |reserved                    |n/a     |n/a                |
 * |---+----------+----------------------------+--------+-------------------|
 * |11 |proc_recv_|Processor recovery occurred |y       |Log message and    |
 * |   |again     |again before bit2 or bit3   |        |continue working.  |
 * |   |          |was cleared                 |        |                   |
 * |---+----------+----------------------------+--------+-------------------|
 * |12-|reserved  |was temperature sensor      |n/a     |n/a                |
 * |15 |          |passed the critical point on|        |                   |
 * |   |          |the way up                  |        |                   |
 * |---+----------+----------------------------+--------+-------------------|
 * |16 |          |SCOM has set a reserved FIR |No      |n/a                |
 * |   |scom_fir_h|bit to cause recovery       |        |                   |
 * |   |m         |                            |        |                   |
 * |---+----------+----------------------------+--------+-------------------|
 * |17 |trig_fir_h|Debug trigger has set a     |No      |n/a                |
 * |   |mi        |reserved FIR bit to cause   |        |                   |
 * |   |          |recovery                    |        |                   |
 * |---+----------+----------------------------+--------+-------------------|
 * |18 |reserved  |reserved                    |n/a     |n/a                |
 * |---+----------+----------------------------+--------+-------------------|
 * |19 |reserved  |reserved                    |n/a     |n/a                |
 * |---+----------+----------------------------+--------+-------------------|
 * |20 |hyp_resour|A hypervisor resource error |y       |Raise attn from    |
 * |   |ce_err    |occurred: data parity error |        |sapphire resulting |
 * |   |          |on, SPRC0:3; SPR_Modereg or |        |xstop.             |
 * |   |          |HMEER.                      |        |                   |
 * |   |          |Note: this bit will cause an|        |                   |
 * |   |          |check_stop when (HV=1, PR=0 |        |                   |
 * |   |          |and EE=0)                   |        |                   |
 * |---+----------+----------------------------+--------+-------------------|
 * |21-|          |if bit 8 is active, the     |No      |We handle it by    |
 * |23 |xscom_stat|reason will be detailed in  |        |Manually reading   |
 * |   |us        |these bits. see chapter 11.1|        |HMER register.     |
 * |   |          |This bits are information   |        |                   |
 * |   |          |only and always masked      |        |                   |
 * |   |          |(mask = '0')                |        |                   |
 * |   |          |If hypervisor intends to use|        |                   |
 * |   |          |this bit, it is responsible |        |                   |
 * |   |          |for clearing it before      |        |                   |
 * |   |          |performing the xscom        |        |                   |
 * |   |          |operation.                  |        |                   |
 * |---+----------+----------------------------+--------+-------------------|
 * |24-|Not       |Not implemented             |n/a     |n/a                |
 * |63 |implemente|                            |        |                   |
 * |   |d         |                            |        |                   |
 * +-- +----------+----------------------------+--------+-------------------+
 *
 * Above HMER bits can be enabled/disabled by modifying
 * SPR_HMEER_HMI_ENABLE_MASK #define in include/processor.h
 * If you modify support for any of the bits listed above, please make sure
 * you change the above table to refelct that.
 *
 * NOTE: Per Dave Larson, never enable 8,9,21-23
 */

static struct lock hmi_lock = LOCK_UNLOCKED;

static int queue_hmi_event(struct OpalHMIEvent *hmi_evt, int recover)
{
	uint64_t *hmi_data;

	/* Don't queue up event if recover == -1 */
	if (recover == -1)
		return 0;

	/* set disposition */
	if (recover == 1)
		hmi_evt->disposition = OpalHMI_DISPOSITION_RECOVERED;
	else if (recover == 0)
		hmi_evt->disposition = OpalHMI_DISPOSITION_NOT_RECOVERED;

	/*
	 * struct OpalHMIEvent is of (3 * 64 bits) size and well packed
	 * structure. Hence use uint64_t pointer to pass entire structure
	 * using 4 params in generic message format.
	 */
	hmi_data = (uint64_t *)hmi_evt;

	/* queue up for delivery to host. */
	return opal_queue_msg(OPAL_MSG_HMI_EVT, NULL, NULL,
				hmi_data[0], hmi_data[1], hmi_data[2]);
}

static int is_capp_recoverable(int chip_id)
{
	uint64_t reg;
	xscom_read(chip_id, CAPP_ERR_STATUS_CTRL, &reg);
	return (reg & PPC_BIT(0)) != 0;
}

static int handle_capp_recoverable(int chip_id)
{
	struct dt_node *np;
	u64 phb_id;
	u32 dt_chip_id;
	struct phb *phb;
	u32 phb_index;

	dt_for_each_compatible(dt_root, np, "ibm,power8-pciex") {
		dt_chip_id = dt_prop_get_u32(np, "ibm,chip-id");
		phb_index = dt_prop_get_u32(np, "ibm,phb-index");
		phb_id = dt_prop_get_u64(np, "ibm,opal-phbid");

		if ((phb_index == 0) && (chip_id == dt_chip_id)) {
			phb = pci_get_phb(phb_id);
			phb->ops->lock(phb);
			phb->ops->set_capp_recovery(phb);
			phb->ops->unlock(phb);
			return 1;
		}
	}
	return 0;
}

static int decode_one_malfunction(int flat_chip_id, struct OpalHMIEvent *hmi_evt)
{
	hmi_evt->severity = OpalHMI_SEV_FATAL;
	hmi_evt->type = OpalHMI_ERROR_MALFUNC_ALERT;

	if (is_capp_recoverable(flat_chip_id)) {
		if (handle_capp_recoverable(flat_chip_id) == 0)
			return 0;

		hmi_evt->severity = OpalHMI_SEV_NO_ERROR;
		hmi_evt->type = OpalHMI_ERROR_CAPP_RECOVERY;
		return 1;
	}
	/* TODO check other FIRs */
	return 0;
}

static int decode_malfunction(struct OpalHMIEvent *hmi_evt)
{
	int i;
	int recover = -1;
	uint64_t malf_alert;

	xscom_read(this_cpu()->chip_id, 0x2020011, &malf_alert);

	for (i = 0; i < 64; i++)
		if (malf_alert & PPC_BIT(i)) {
			recover = decode_one_malfunction(i, hmi_evt);
			xscom_write(this_cpu()->chip_id, 0x02020011, ~PPC_BIT(i));
		}

	return recover;
}

int handle_hmi_exception(uint64_t hmer, struct OpalHMIEvent *hmi_evt)
{
	int recover = 1;
	uint64_t tfmr;

	printf("HMI: Received HMI interrupt: HMER = 0x%016llx\n", hmer);
	if (hmi_evt)
		hmi_evt->hmer = hmer;
	if (hmer & SPR_HMER_PROC_RECV_DONE) {
		hmer &= ~SPR_HMER_PROC_RECV_DONE;
		if (hmi_evt) {
			hmi_evt->severity = OpalHMI_SEV_NO_ERROR;
			hmi_evt->type = OpalHMI_ERROR_PROC_RECOV_DONE;
			queue_hmi_event(hmi_evt, recover);
		}
		printf("HMI: Processor recovery Done.\n");
	}
	if (hmer & SPR_HMER_PROC_RECV_ERROR_MASKED) {
		hmer &= ~SPR_HMER_PROC_RECV_ERROR_MASKED;
		if (hmi_evt) {
			hmi_evt->severity = OpalHMI_SEV_NO_ERROR;
			hmi_evt->type = OpalHMI_ERROR_PROC_RECOV_MASKED;
			queue_hmi_event(hmi_evt, recover);
		}
		printf("HMI: Processor recovery Done (masked).\n");
	}
	if (hmer & SPR_HMER_PROC_RECV_AGAIN) {
		hmer &= ~SPR_HMER_PROC_RECV_AGAIN;
		if (hmi_evt) {
			hmi_evt->severity = OpalHMI_SEV_NO_ERROR;
			hmi_evt->type = OpalHMI_ERROR_PROC_RECOV_DONE_AGAIN;
			queue_hmi_event(hmi_evt, recover);
		}
		printf("HMI: Processor recovery occurred again before"
			"bit2 was cleared\n");
	}
	/* Assert if we see malfunction alert, we can not continue. */
	if (hmer & SPR_HMER_MALFUNCTION_ALERT) {
		hmer &= ~SPR_HMER_MALFUNCTION_ALERT;
		recover = 0;

		if (hmi_evt) {
			recover = decode_malfunction(hmi_evt);
			queue_hmi_event(hmi_evt, recover);
		}
	}

	/* Assert if we see Hypervisor resource error, we can not continue. */
	if (hmer & SPR_HMER_HYP_RESOURCE_ERR) {
		hmer &= ~SPR_HMER_HYP_RESOURCE_ERR;
		recover = 0;
		if (hmi_evt) {
			hmi_evt->severity = OpalHMI_SEV_FATAL;
			hmi_evt->type = OpalHMI_ERROR_HYP_RESOURCE;
			queue_hmi_event(hmi_evt, recover);
		}
	}

	/*
	 * Assert for now for all TOD errors. In future we need to decode
	 * TFMR and take corrective action wherever required.
	 */
	if (hmer & SPR_HMER_TFAC_ERROR) {
		tfmr = mfspr(SPR_TFMR);		/* save original TFMR */
		hmer &= ~SPR_HMER_TFAC_ERROR;
		recover = chiptod_recover_tb_errors();
		if (hmi_evt) {
			hmi_evt->severity = OpalHMI_SEV_ERROR_SYNC;
			hmi_evt->type = OpalHMI_ERROR_TFAC;
			hmi_evt->tfmr = tfmr;
			queue_hmi_event(hmi_evt, recover);
		}
	}
	if (hmer & SPR_HMER_TFMR_PARITY_ERROR) {
		tfmr = mfspr(SPR_TFMR);		/* save original TFMR */
		hmer &= ~SPR_HMER_TFMR_PARITY_ERROR;
		recover = 0;
		if (hmi_evt) {
			hmi_evt->severity = OpalHMI_SEV_FATAL;
			hmi_evt->type = OpalHMI_ERROR_TFMR_PARITY;
			hmi_evt->tfmr = tfmr;
			queue_hmi_event(hmi_evt, recover);
		}
	}

	/*
	 * HMER bits are sticky, once set to 1 they remain set to 1 until
	 * they are set to 0. Reset the error source bit to 0, otherwise
	 * we keep getting HMI interrupt again and again.
	 */
	mtspr(SPR_HMER, hmer);
	return recover;
}

static int64_t opal_handle_hmi(void)
{
	uint64_t hmer;
	int rc = OPAL_SUCCESS;
	struct OpalHMIEvent hmi_evt;

	memset(&hmi_evt, 0, sizeof(struct OpalHMIEvent));
	hmi_evt.version = OpalHMIEvt_V1;

	lock(&hmi_lock);
	hmer = mfspr(SPR_HMER);		/* Get HMER register value */
	handle_hmi_exception(hmer, &hmi_evt);
	unlock(&hmi_lock);

	return rc;
}
opal_call(OPAL_HANDLE_HMI, opal_handle_hmi, 0);
