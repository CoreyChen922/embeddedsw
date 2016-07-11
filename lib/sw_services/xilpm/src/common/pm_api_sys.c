/******************************************************************************
*
* Copyright (C) 2015-2016 Xilinx, Inc.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* Use of the Software is limited solely to applications:
* (a) running on a Xilinx device, or
* (b) that interact with a Xilinx device through a bus or interconnect.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* XILINX  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
* Except as contained in this notice, the name of the Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Xilinx.
*
******************************************************************************/

#include "pm_client.h"
#include "pm_common.h"
#include "pm_api_sys.h"
#include "pm_callbacks.h"

/** @name Payload Packets
 *
 * Assigning of argument values into array elements.
 * pause and pm_dbg are used for debugging and should be removed in
 * final version.
 * @{
 */
#define PACK_PAYLOAD(pl, arg0, arg1, arg2, arg3, arg4, arg5)		\
	pl[0] = (u32)arg0;						\
	pl[1] = (u32)arg1;						\
	pl[2] = (u32)arg2;						\
	pl[3] = (u32)arg3;						\
	pl[4] = (u32)arg4;						\
	pl[5] = (u32)arg5;						\
	pm_dbg("%s(%x, %x, %x, %x, %x)\n", __func__, arg1, arg2, arg3, arg4, arg5);

#define PACK_PAYLOAD0(pl, api_id) \
	PACK_PAYLOAD(pl, api_id, 0, 0, 0, 0, 0)
#define PACK_PAYLOAD1(pl, api_id, arg1) \
	PACK_PAYLOAD(pl, api_id, arg1, 0, 0, 0, 0)
#define PACK_PAYLOAD2(pl, api_id, arg1, arg2) \
	PACK_PAYLOAD(pl, api_id, arg1, arg2, 0, 0, 0)
#define PACK_PAYLOAD3(pl, api_id, arg1, arg2, arg3) \
	PACK_PAYLOAD(pl, api_id, arg1, arg2, arg3, 0, 0)
#define PACK_PAYLOAD4(pl, api_id, arg1, arg2, arg3, arg4) \
	PACK_PAYLOAD(pl, api_id, arg1, arg2, arg3, arg4, 0)
#define PACK_PAYLOAD5(pl, api_id, arg1, arg2, arg3, arg4, arg5) \
	PACK_PAYLOAD(pl, api_id, arg1, arg2, arg3, arg4, arg5)
/*@}*/

/****************************************************************************/
/**
 * @brief  Initialize xilpm library
 *
 * @param  IpiInst Pointer to IPI driver instance
 *
 * @return Returns status, either success or error+reason
 *
 * @note   None
 *
 ****************************************************************************/
XStatus XPm_InitXilpm(XIpiPsu *IpiInst)
{
	XStatus status = XST_SUCCESS;

	if (NULL == IpiInst) {
		pm_dbg("ERROR passing NULL pointer to %s\n", __func__);
		status = XST_INVALID_PARAM;
		goto done;
	}

	primary_master->ipi = IpiInst;
done:
	return status;
}

/****************************************************************************/
/**
 * @brief  Checks for reason of boot
 * <br>
 * Function returns information about the boot reason.
 * If the boot is not a system startup but a resume,
 * power down request bitfield for this processor will be cleared.
 *
 * @return Returns processor boot status
 *
 * @note   None
 *
 ****************************************************************************/
enum XPmBootStatus XPm_GetBootStatus()
{
	u32 pwrdn_req = pm_read(MASTER_PWRCTL);
	if (0 != (pwrdn_req & primary_master->pwrdn_mask)) {
		pm_write(MASTER_PWRCTL, pwrdn_req & (~primary_master->pwrdn_mask));
		return PM_RESUME;
	} else {
		return PM_INITIAL_BOOT;
	}
}

/****************************************************************************/
/**
 * @brief  Finalize suspend procedure
 * <br>
 * Function waits for PMU to finish all previous API requests sent by the PU
 * and performs client specific actions to finish suspend procedure
 * (e.g. execution of wfi instruction on A53 and R5 processors).
 * This function should not return if the suspend procedure is successful.
 *
 * @note   None
 *
 ****************************************************************************/
void XPm_SuspendFinalize(void)
{
	XStatus status;

	/**
	 * Wait until previous IPI request is handled by the PMU
	 * If PMU is busy, keep trying until PMU becomes responsive
	 */
	do {
		status = XIpiPsu_PollForAck(primary_master->ipi,
					    IPI_PMU_PM_INT_MASK,
					    PM_IPI_TIMEOUT);
		if (status != XST_SUCCESS) {
			pm_dbg("ERROR timed out while waiting for PMU to"
			       " finish processing previous PM-API call\n");
		}
	} while (XST_SUCCESS != status);

	XPm_ClientSuspendFinalize();
}

/****************************************************************************/
/**
 * @brief  Sends IPI request to the PMU
 *
 * @param  master  Pointer to the master who is initiating request
 * @param  payload API id and call arguments to be written in IPI buffer
 *
 * @return Returns status, either success or error+reason
 *
 * @note   None
 *
 ****************************************************************************/
static XStatus pm_ipi_send(struct XPm_Master *const master,
			   u32 payload[PAYLOAD_ARG_CNT])
{
	XStatus status;

	status = XIpiPsu_PollForAck(master->ipi, IPI_PMU_PM_INT_MASK,
				    PM_IPI_TIMEOUT);
	if (status != XST_SUCCESS) {
		pm_dbg("%s: ERROR: Timeout expired\n", __func__);
		goto done;
	}

	status = XIpiPsu_WriteMessage(master->ipi, IPI_PMU_PM_INT_MASK,
				      payload, PAYLOAD_ARG_CNT,
				      XIPIPSU_BUF_TYPE_MSG);
	if (status != XST_SUCCESS) {
		pm_dbg("xilpm: ERROR writing to IPI request buffer\n");
		goto done;
	}

	status = XIpiPsu_TriggerIpi(master->ipi, IPI_PMU_PM_INT_MASK);
done:
	return status;
}

/****************************************************************************/
/**
 * @brief  Reads IPI response after PMU has handled interrupt
 *
 * @param  master Pointer to the master who is waiting and reading response
 * @param  value1 Used to return value from 2nd IPI buffer element (optional)
 * @param  value2 Used to return value from 3rd IPI buffer element (optional)
 * @param  value3 Used to return value from 4th IPI buffer element (optional)
 *
 * @return Returns status, either success or error+reason
 *
 * @note   None
 *
 ****************************************************************************/
static XStatus pm_ipi_buff_read32(struct XPm_Master *const master,
				  u32 *value1, u32 *value2, u32 *value3)
{
	u32 response[RESPONSE_ARG_CNT];
	XStatus status;

	/* Wait until current IPI interrupt is handled by PMU */
	status = XIpiPsu_PollForAck(master->ipi, IPI_PMU_PM_INT_MASK,
				  PM_IPI_TIMEOUT);

	if (status != XST_SUCCESS) {
		pm_dbg("%s: ERROR: Timeout expired\n", __func__);
		goto done;
	}

	status = XIpiPsu_ReadMessage(master->ipi, IPI_PMU_PM_INT_MASK,
				     response, RESPONSE_ARG_CNT,
				     XIPIPSU_BUF_TYPE_RESP);

	if (status != XST_SUCCESS) {
		pm_dbg("xilpm: ERROR reading from PMU's IPI response buffer\n");
		goto done;
	}

	/*
	 * Read response from IPI buffer
	 * buf-0: success or error+reason
	 * buf-1: value1
	 * buf-2: value2
	 * buf-3: value3
	 */
	if (NULL != value1)
		*value1 = response[1];
	if (NULL != value2)
		*value2 = response[2];
	if (NULL != value3)
		*value3 = response[3];

	status = response[0];
done:
	return status;
}

/****************************************************************************/
/**
 * @brief  PM call for master to suspend itself
 *
 * @param  nid     Node id of the master or subsystem
 * @param  latency Requested maximum wakeup latency (not supported)
 * @param  state   Requested state (not supported)
 * @param  address Address from which processor should resume
 *
 * @return Returns status, either success or error+reason
 *
 * @note   This is a blocking call, it will return only once PMU has responded
 *
 ****************************************************************************/
XStatus XPm_SelfSuspend(const enum XPmNodeId nid,
			const u32 latency,
			const u8 state,
			const u64 address)
{
	XStatus ret;
	u32 payload[PAYLOAD_ARG_CNT];

	struct XPm_Master *master = pm_get_master_by_node(nid);
	if (NULL == master) {
		/*
		 * If a subsystem node ID (APU or RPU) was passed then
		 * the master to be used is the primary master.
		 * E.g. for the APU the primary master is APU0
		 */
		if (subsystem_node == nid) {
			master = primary_master;
		} else {
			return XST_INVALID_PARAM;
		}
	}
	/*
	 * Do client specific suspend operations
	 * (e.g. disable interrupts and set powerdown request bit)
	 */
	XPm_ClientSuspend(master);

	/* Send request to the PMU */
	PACK_PAYLOAD5(payload, PM_SELF_SUSPEND, nid, latency, state, (u32)address,
		     (u32)(address >> 32));
	ret = pm_ipi_send(master, payload);
	if (XST_SUCCESS != ret)
		return ret;
	/* Wait for PMU to finish handling request */
	return pm_ipi_buff_read32(master, NULL, NULL, NULL);
}

/****************************************************************************/
/**
 * @brief  PM call to request for another PU or subsystem to be
 * suspended gracefully.
 *
 * @param  target  Node id of the targeted PU or subsystem
 * @param  ack     Flag to specify whether acknowledge is requested
 * @param  latency Requested wakeup latency (not supported)
 * @param  state   Requested state (not supported)
 *
 * @return Returns status, either success or error+reason
 *
 * @note   None
 *
 ****************************************************************************/
XStatus XPm_RequestSuspend(const enum XPmNodeId target,
			   const enum XPmRequestAck ack,
			   const u32 latency, const u8 state)
{
	XStatus ret;
	u32 payload[PAYLOAD_ARG_CNT];

	/* Send request to the PMU */
	PACK_PAYLOAD4(payload, PM_REQUEST_SUSPEND, target, ack, latency, state);
	ret = pm_ipi_send(primary_master, payload);

	if ((XST_SUCCESS == ret) && (REQUEST_ACK_BLOCKING == ack))
		return pm_ipi_buff_read32(primary_master, NULL, NULL, NULL);
	else
		return ret;
}

/****************************************************************************/
/**
 * @brief  PM call for master to wake up selected master or subsystem
 *
 * @param  target     Node id of the targeted PU or subsystem
 * @param  setAddress Specifies whether the start address argument is being
 *   passed.
 * @param  address    Parameter_Description
 * @param  ack        Flag to specify whether acknowledge requested
 *
 * @return Returns status, either success or error+reason
 *
 * @note   None
 *
 ****************************************************************************/
XStatus XPm_RequestWakeUp(const enum XPmNodeId target,
			  const bool setAddress,
			  const u64 address,
			  const enum XPmRequestAck ack)
{
	XStatus ret;
	u32 payload[PAYLOAD_ARG_CNT];
	u64 encodedAddress;
	struct XPm_Master *master = pm_get_master_by_node(target);

	XPm_ClientWakeup(master);

	/* encode set Address into 1st bit of address */
	encodedAddress = address | !!setAddress;

	/* Send request to the PMU */
	PACK_PAYLOAD4(payload, PM_REQUEST_WAKEUP, target, (u32)encodedAddress,
		     (u32)(encodedAddress >> 32), ack);
	ret = pm_ipi_send(primary_master, payload);

	if ((XST_SUCCESS == ret) && (REQUEST_ACK_BLOCKING == ack))
		return pm_ipi_buff_read32(primary_master, NULL, NULL, NULL);
	else
		return ret;
}

/****************************************************************************/
/**
 * @brief  PM call to request for another PU or subsystem to be
 * powered down forcefully
 *
 * @param  target Node id of the targeted PU or subsystem
 * @param  ack    Flag to specify whether acknowledge is requested
 *
 * @return Returns status, either success or error+reason
 *
 * @note   None
 *
 ****************************************************************************/
XStatus XPm_ForcePowerDown(const enum XPmNodeId target,
			   const enum XPmRequestAck ack)
{
	XStatus ret;
	u32 payload[PAYLOAD_ARG_CNT];

	/* Send request to the PMU */
	PACK_PAYLOAD2(payload, PM_FORCE_POWERDOWN, target, ack);
	ret = pm_ipi_send(primary_master, payload);

	if ((XST_SUCCESS == ret) && (REQUEST_ACK_BLOCKING == ack))
		return pm_ipi_buff_read32(primary_master, NULL, NULL, NULL);
	else
		return ret;
}

/****************************************************************************/
/**
 * @brief  PM call to announce that a prior suspend request is to be aborted.
 *
 * @param  reason Reason for the abort
 *
 * @return Returns status, either success or error+reason
 *
 * @note   Calling PU expects the PMU to abort the initiated suspend procedure.
 * This is a non-blocking call without any acknowledge.
 *
 ****************************************************************************/
XStatus XPm_AbortSuspend(const enum XPmAbortReason reason)
{
	XStatus status;
	u32 payload[PAYLOAD_ARG_CNT];

	/* Send request to the PMU */
	PACK_PAYLOAD2(payload, PM_ABORT_SUSPEND, reason, primary_master->node_id);
	status = pm_ipi_send(primary_master, payload);
	if (XST_SUCCESS == status) {
		/* Wait for PMU to finish handling request */
		status = XIpiPsu_PollForAck(primary_master->ipi,
					IPI_PMU_PM_INT_MASK, PM_IPI_TIMEOUT);
		if (status != XST_SUCCESS) {
			pm_dbg("%s: ERROR: Timeout expired\n", __func__);
		}
	}

	/*
	 * Do client specific abort suspend operations
	 * (e.g. enable interrupts and clear powerdown request bit)
	 */
	XPm_ClientAbortSuspend();

	return status;
}

/****************************************************************************/
/**
 * @brief  PM call to specify the wakeup source while suspended
 *
 * @param  target    Node id of the targeted PU or subsystem
 * @param  wkup_node Node id of the wakeup peripheral
 * @param  enable    Enable or disable the specified peripheral as wake source
 *
 * @return Returns status, either success or error+reason
 *
 * @note   None
 *
 ****************************************************************************/
XStatus XPm_SetWakeUpSource(const enum XPmNodeId target,
			    const enum XPmNodeId wkup_node,
			    const u8 enable)
{
	u32 payload[PAYLOAD_ARG_CNT];
	PACK_PAYLOAD3(payload, PM_SET_WAKEUP_SOURCE, target, wkup_node, enable);
	return pm_ipi_send(primary_master, payload);
}

/****************************************************************************/
/**
 * @brief  PM call to request a system Shutdown or Restart
 *
 * @param  restart Shutdown or Restart ? 0 for Shutdown, 1 for Restart
 *
 * @return Returns status, either success or error+reason
 *
 * @note   None
 *
 ****************************************************************************/
XStatus XPm_SystemShutdown(const u8 restart)
{
	u32 payload[PAYLOAD_ARG_CNT];
	PACK_PAYLOAD1(payload, PM_SYSTEM_SHUTDOWN, restart);
	return pm_ipi_send(primary_master, payload);
}

/**
 * APIs for managing PM slaves:
 */

/****************************************************************************/
/**
 * @brief  PM call to request a node with specifc capabilities
 *
 * @param  node         Node id of the slave
 * @param  capabilities Requested capabilities of the slave
 * @param  qos          Quality of service (not supported)
 * @param  ack          Flag to specify whether acknowledge is requested
 *
 * @return Returns status, either success or error+reason
 *
 * @note   None
 *
 ****************************************************************************/
XStatus XPm_RequestNode(const enum XPmNodeId node,
			const u32 capabilities,
			const u32 qos,
			const enum XPmRequestAck ack)
{
	XStatus ret;
	u32 payload[PAYLOAD_ARG_CNT];

	PACK_PAYLOAD4(payload, PM_REQUEST_NODE, node, capabilities, qos, ack);
	ret = pm_ipi_send(primary_master, payload);

	if ((XST_SUCCESS == ret) && (REQUEST_ACK_BLOCKING == ack))
		return pm_ipi_buff_read32(primary_master, NULL, NULL, NULL);
	else
		return ret;
}

/****************************************************************************/
/**
 * @brief  PM call to set requirement for PM slaves
 *
 * @param  nid          Node id of the slave
 * @param  capabilities Requested capabilities of the slave
 * @param  qos          Quality of service (not supported)
 * @param  ack          Flag to specify whether acknowledge is requested
 *
 * @return Returns status, either success or error+reason
 *
 * @note   This API function is to be used for slaves a PU already
 * has requested
 *
 ****************************************************************************/
XStatus XPm_SetRequirement(const enum XPmNodeId nid,
			   const u32 capabilities,
			   const u32 qos,
			   const enum XPmRequestAck ack)
{
	XStatus ret;
	u32 payload[PAYLOAD_ARG_CNT];
	PACK_PAYLOAD4(payload, PM_SET_REQUIREMENT, nid, capabilities, qos, ack);
	ret = pm_ipi_send(primary_master, payload);

	if ((XST_SUCCESS == ret) && (REQUEST_ACK_BLOCKING == ack))
		return pm_ipi_buff_read32(primary_master, NULL, NULL, NULL);
	else
		return ret;
}

/****************************************************************************/
/**
 * @brief  PM call to release a node
 *
 * @param  node Node id of the slave
 *
 * @return Returns status, either success or error+reason
 *
 * @note   None
 *
 ****************************************************************************/
XStatus XPm_ReleaseNode(const enum XPmNodeId node)
{
	u32 payload[PAYLOAD_ARG_CNT];
	PACK_PAYLOAD1(payload, PM_RELEASE_NODE, node);
	return pm_ipi_send(primary_master, payload);
}

/****************************************************************************/
/**
 * @brief  PM call to set wakeup latency requirements
 *
 * @param  node    Node id of the slave
 * @param  latency Requested maximum wakeup latency
 *
 * @return Returns status, either success or error+reason
 *
 * @note   None
 *
 ****************************************************************************/
XStatus XPm_SetMaxLatency(const enum XPmNodeId node,
			  const u32 latency)
{
	u32 payload[PAYLOAD_ARG_CNT];

	/* Send request to the PMU */
	PACK_PAYLOAD2(payload, PM_SET_MAX_LATENCY, node, latency);
	return pm_ipi_send(primary_master, payload);
}

/**
 * Callback API functions
 */
struct pm_init_suspend pm_susp = {
	.received = false,
/* initialization of other fields is irrelevant while 'received' is false */
};

struct pm_acknowledge pm_ack = {
	.received = false,
/* initialization of other fields is irrelevant while 'received' is false */
};

/****************************************************************************/
/**
 * @brief  callback to handle request made by PMU for initiating suspend of
 * this PU. If PU does not suspend itself within given timeout,
 * PMU might force it to power down.
 *
 * @param  reason  Suspend reason
 * @param  latency Maximum allowed latency for waking up
 *  (determines lowest state)
 * @param  state   State to which the PU should suspend
 * @param  timeout How much time this PU have to suspend itself
 *
 * @return None
 *
 * @note   This function executes in interrupt context. The function itself
 * should never invoke PM API calls that could block (because of performance
 * reasons) or execution of wfi (impossible). Therefore, this function should
 * schedule suspend procedure to be done out of the interrupt context.
 * In this case, it is simply done by filling the structure with arguments
 * of the call and marking that the init suspend request is received.
 *
 ****************************************************************************/
void XPm_InitSuspendCb(const enum XPmSuspendReason reason,
		       const u32 latency,
		       const u32 state,
		       const u32 timeout)
{
	if (true == pm_susp.received) {
		pm_dbg("WARNING: dropping unhandled init suspend request!\n");
		pm_dbg("Dropped %s (%d, %d, %d, %d)\n", __func__, pm_susp.reason,
			pm_susp.latency, pm_susp.state, pm_susp.timeout);
	}
	pm_dbg("%s (%d, %d, %d, %d)\n", __func__, reason, latency, state, timeout);

	pm_susp.reason = reason;
	pm_susp.latency = latency;
	pm_susp.state = state;
	pm_susp.timeout = timeout;
	pm_susp.received = true;
}

/****************************************************************************/
/**
 * @brief  Callback to handle acknowledge from PMU
 *
 * @param  node    Node about which the acknowledge is about
 * @param  status  Acknowledged status
 * @param  oppoint Operating point of the node in question
 *
 * @return None
 *
 * @note   None
 *
 ****************************************************************************/
void XPm_AcknowledgeCb(const enum XPmNodeId node,
		       const XStatus status,
		       const u32 oppoint)
{
	if (true == pm_ack.received) {
		pm_dbg("WARNING: dropping unhandled acknowledge!\n");
		pm_dbg("Dropped %s (%d, %d, %d)\n", __func__, pm_ack.node,
			pm_ack.status, pm_ack.opp);
	}
	pm_dbg("%s (%d, %d, %d)\n", __func__, node, status, oppoint);

	pm_ack.node = node;
	pm_ack.status = status;
	pm_ack.opp = oppoint;
	pm_ack.received = true;
}

/****************************************************************************/
/**
 * @brief  Nofity Callback. This function is called by the power management
 *  controller if an event for which the PU was registered has occurred.
 *
 * @param  node    ID of the node the event notification is related to.
 * @param  event   ID of the event
 * @param  oppoint Current operating state of the node.
 *
 * @return None
 *
 * @note   None
 *
 ****************************************************************************/
void XPm_NotifyCb(const enum XPmNodeId node,
		  const u32 event,
		  const u32 oppoint)
{
	pm_dbg("%s (%d, %d, %d)\n", __func__, node, event, oppoint);
	XPm_NotifierProcessEvent(node, event, oppoint);
}

/**
 * Miscellaneous API functions
 */

/****************************************************************************/
/**
 * @brief  Get version number of PMU PM firmware
 *
 * @param  version 32-bit version number of PMU Power Management Firmware
 *
 * @return Returns status, either success or error+reason
 *
 * @note   None
 *
 ****************************************************************************/
XStatus XPm_GetApiVersion(u32 *version)
{
	XStatus ret;
	u32 payload[PAYLOAD_ARG_CNT];

	/* Send request to the PMU */
	PACK_PAYLOAD0(payload, PM_GET_API_VERSION);
	ret = pm_ipi_send(primary_master, payload);

	if (XST_SUCCESS != ret)
		return ret;

	/* Return result from IPI return buffer */
	return pm_ipi_buff_read32(primary_master, version, NULL, NULL);
}

/****************************************************************************/
/**
 * @brief  PM call to request a node's current power state
 *
 * @param  node       Node id of the node to be queried
 * @param  nodestatus Struct to be passed by caller to be populated
 *  with node status
 *
 * @return Returns status, either success or error+reason
 *
 * @note   None
 *
 ****************************************************************************/
XStatus XPm_GetNodeStatus(const enum XPmNodeId node,
			  XPm_NodeStatus *const nodestatus)
{
	XStatus ret;
	u32 payload[PAYLOAD_ARG_CNT];

	/* Send request to the PMU */
	PACK_PAYLOAD1(payload, PM_GET_NODE_STATUS, node);
	ret = pm_ipi_send(primary_master, payload);

	if (XST_SUCCESS != ret)
		return ret;

	/* Return result from IPI return buffer */
	return pm_ipi_buff_read32(primary_master, &nodestatus->status,
				  &nodestatus->requirements,
				  &nodestatus->usage);
}

/****************************************************************************/
/**
 * @brief  PM call to request operating characteristics of a node
 *
 * @param  node   Node id of the slave
 * @param  type   Type of the operating characteristics
 * @param  result Returns the operating characteristic for the requested node,
 *  specified by the type
 *
 * @return Returns status, either success or error/reason
 *
 * @note   None
 *
 ****************************************************************************/
XStatus XPm_GetOpCharacteristic(const enum XPmNodeId node,
				const enum XPmOpCharType type,
				u32* const result)
{
	XStatus ret;
	u32 payload[PAYLOAD_ARG_CNT];

	/* Send request to the PMU */
	PACK_PAYLOAD2(payload, PM_GET_OP_CHARACTERISTIC, node, type);
	ret = pm_ipi_send(primary_master, payload);

	if (XST_SUCCESS != ret)
		return ret;

	/* Return result from IPI return buffer */
	return pm_ipi_buff_read32(primary_master, result, NULL, NULL);
}

/****************************************************************************/
/**
 * @brief  Assert/release reset line
 *
 * @param  reset  Reset line
 * @param  assert Identifies action: (release, assert, Pulse)
 *
 * @return Returns status, either success or error+reason
 *
 * @note   None
 *
 ****************************************************************************/
XStatus XPm_ResetAssert(const enum XPmReset reset,
			const enum XPmResetAction assert)
{
       XStatus ret;
       u32 payload[PAYLOAD_ARG_CNT];

       /* Send request to the PMU */
       PACK_PAYLOAD2(payload, PM_RESET_ASSERT, reset, assert);
       ret = pm_ipi_send(primary_master, payload);

       if (XST_SUCCESS != ret)
               return ret;

       /* Return result from IPI return buffer */
       return pm_ipi_buff_read32(primary_master, NULL, NULL, NULL);
}

/****************************************************************************/
/**
 * @brief  Get current status of a given reset line
 *
 * @param  reset  Reset line
 * @param  status Status of specified reset (true - asserted, false - released)
 *
 * @return Returns status, either success or error+reason
 *
 * @note   None
 *
 ****************************************************************************/
XStatus XPm_ResetGetStatus(const enum XPmReset reset, u32 *status)
{
	XStatus ret;
	u32 payload[PAYLOAD_ARG_CNT];

	/* Send request to the PMU */
	PACK_PAYLOAD1(payload, PM_RESET_GET_STATUS, reset);
	ret = pm_ipi_send(primary_master, payload);

	if (XST_SUCCESS != ret)
		return ret;

	/* Return result from IPI return buffer */
	return pm_ipi_buff_read32(primary_master, status, NULL, NULL);
}

/****************************************************************************/
/**
 * @brief  Register notifier for PM events
 *
 * @param  notifier Pointer to data block to be linked in the notifier list
 *  (includes node ID, event ID and wake flag to be passed to the PMU)
 *
 * @return Returns status, either success or error/reason
 *
 * @note   None
 *
 ****************************************************************************/
XStatus XPm_RegisterNotifier(XPm_Notifier* const notifier)
{
	XStatus ret;
	u32 payload[PAYLOAD_ARG_CNT];

	if (!notifier) {
		pm_dbg("%s ERROR: NULL notifier pointer\n", __func__);
		return XST_INVALID_PARAM;
	}

	/* Send request to the PMU */
	PACK_PAYLOAD4(payload, PM_REGISTER_NOTIFIER, notifier->node,
		      notifier->event, notifier->flags, 1);
	ret = pm_ipi_send(primary_master, payload);

	if (XST_SUCCESS != ret)
		return ret;

	ret = pm_ipi_buff_read32(primary_master, NULL, NULL, NULL);

	if (XST_SUCCESS != ret)
		return ret;

	/* Add notifier in the list only if PMU has it registered */
	return XPm_NotifierAdd(notifier);
}

/****************************************************************************/
/**
 * @brief  Unregister notifier for PM events
 *
 * @param  notifier Pointer to data block to be removed from the notifier list
 *  (includes node ID, event ID and wake flag to be passed to the PMU)
 *
 * @return Returns status, either success or error/reason
 *
 * @note   None
 *
 ****************************************************************************/
XStatus XPm_UnregisterNotifier(XPm_Notifier* const notifier)
{
	XStatus ret;
	u32 payload[PAYLOAD_ARG_CNT];

	if (!notifier) {
		pm_dbg("%s ERROR: NULL notifier pointer\n", __func__);
		return XST_INVALID_PARAM;
	}

	/*
	 * Remove first the notifier from the list. If it's not in the list
	 * report an error, and don't trigger PMU since it don't have it
	 * registered either.
	 */
	ret = XPm_NotifierRemove(notifier);
	if (XST_SUCCESS != ret)
		return ret;

	/* Send request to the PMU */
	PACK_PAYLOAD4(payload, PM_REGISTER_NOTIFIER, notifier->node,
		      notifier->event, 0, 0);
	ret = pm_ipi_send(primary_master, payload);

	return pm_ipi_buff_read32(primary_master, NULL, NULL, NULL);
}

/****************************************************************************/
/**
 * @brief  Perform write to protected mmio
 *
 * @param  address Address to write to
 * @param  mask    Mask to apply
 * @param  value   Value to write
 *
 * @return Returns status, either success or error+reason
 *
 * @note   This function provides access to PM-related control registers
 *  that may not be directly accessible by a particular PU.
 *
 ****************************************************************************/
XStatus XPm_MmioWrite(const u32 address, const u32 mask, const u32 value)
{
	XStatus status;
	u32 payload[PAYLOAD_ARG_CNT];

	/* Send request to the PMU */
	PACK_PAYLOAD3(payload, PM_MMIO_WRITE, address, mask, value);
	status = pm_ipi_send(primary_master, payload);

	if (XST_SUCCESS != status)
		return status;

	/* Return result from IPI return buffer */
	return pm_ipi_buff_read32(primary_master, NULL, NULL, NULL);
}

/****************************************************************************/
/**
 * @brief  Read value from protected mmio
 *
 * @param  address Address to write to
 * @param  value   Value to write
 *
 * @return Returns status, either success or error+reason
 *
 * @note   This function provides access to PM-related control registers
 *  that may not be directly accessible by a particular PU.
 *
 ****************************************************************************/
XStatus XPm_MmioRead(const u32 address, u32 *const value)
{
	XStatus status;
	u32 payload[PAYLOAD_ARG_CNT];

	/* Send request to the PMU */
	PACK_PAYLOAD1(payload, PM_MMIO_READ, address);
	status = pm_ipi_send(primary_master, payload);

	if (XST_SUCCESS != status)
		return status;

	/* Return result from IPI return buffer */
	return pm_ipi_buff_read32(primary_master, value, NULL, NULL);
}
