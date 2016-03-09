/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef SPDK_NVME_H
#define SPDK_NVME_H

#include <stddef.h>
#include "nvme_spec.h"

/** \file
 *
 */

#define NVME_DEFAULT_RETRY_COUNT	(4)
extern int32_t		nvme_retry_count;

#ifdef __cplusplus
extern "C" {
#endif

/** \brief Opaque handle to a controller. Obtained by calling nvme_attach(). */
struct nvme_controller;

/**
 * \brief Attaches specified device to the NVMe driver.
 *
 * On success, the nvme_controller handle is valid for other nvme_ctrlr_* functions.
 * On failure, the return value will be NULL.
 *
 * This function should be called from a single thread while no other threads or drivers
 * are actively using the NVMe device.
 *
 * To stop using the the controller and release its associated resources,
 * call \ref nvme_detach with the nvme_controller instance returned by this function.
 */
struct nvme_controller *nvme_attach(void *devhandle);

/**
 * \brief Detaches specified device returned by \ref nvme_attach() from the NVMe driver.
 *
 * On success, the nvme_controller handle is no longer valid.
 *
 * This function should be called from a single thread while no other threads
 * are actively using the NVMe device.
 *
 */
int nvme_detach(struct nvme_controller *ctrlr);

/**
 * \brief Perform a full hardware reset of the NVMe controller.
 *
 * This function should be called from a single thread while no other threads
 * are actively using the NVMe device.
 *
 * Any pointers returned from nvme_ctrlr_get_ns() and nvme_ns_get_data() may be invalidated
 * by calling this function.  The number of namespaces as returned by nvme_ctrlr_get_num_ns() may
 * also change.
 */
int nvme_ctrlr_reset(struct nvme_controller *ctrlr);

/**
 * \brief Get the identify controller data as defined by the NVMe specification.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
const struct nvme_controller_data *nvme_ctrlr_get_data(struct nvme_controller *ctrlr);

/**
 * \brief Get the number of namespaces for the given NVMe controller.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 * This is equivalent to calling nvme_ctrlr_get_data() to get the
 * nvme_controller_data and then reading the nn field.
 *
 */
uint32_t nvme_ctrlr_get_num_ns(struct nvme_controller *ctrlr);

/**
 * \brief Determine if a particular log page is supported by the given NVMe controller.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 * \sa nvme_ctrlr_cmd_get_log_page()
 */
bool nvme_ctrlr_is_log_page_supported(struct nvme_controller *ctrlr, uint8_t log_page);

/**
 * \brief Determine if a particular feature is supported by the given NVMe controller.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 * \sa nvme_ctrlr_cmd_get_feature()
 */
bool nvme_ctrlr_is_feature_supported(struct nvme_controller *ctrlr, uint8_t feature_code);

/**
 * Signature for callback function invoked when a command is completed.
 *
 * The nvme_completion parameter contains the completion status.
 */
typedef void (*nvme_cb_fn_t)(void *, const struct nvme_completion *);

/**
 * Signature for callback function invoked when an asynchronous error
 *  request command is completed.
 *
 * The aer_cb_arg parameter is set to the context specified by
 *  nvme_register_aer_callback().
 * The nvme_completion parameter contains the completion status of the
 *  asynchronous event request that was completed.
 */
typedef void (*nvme_aer_cb_fn_t)(void *aer_cb_arg,
				 const struct nvme_completion *);

void nvme_ctrlr_register_aer_callback(struct nvme_controller *ctrlr,
				      nvme_aer_cb_fn_t aer_cb_fn,
				      void *aer_cb_arg);

/**
 * \brief Send the given NVM I/O command to the NVMe controller.
 *
 * This is a low level interface for submitting I/O commands directly. Prefer
 * the nvme_ns_cmd_* functions instead. The validity of the command will
 * not be checked!
 *
 * When constructing the nvme_command it is not necessary to fill out the PRP
 * list/SGL or the CID. The driver will handle both of those for you.
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 *
 */
int nvme_ctrlr_cmd_io_raw(struct nvme_controller *ctrlr,
			  struct nvme_command *cmd,
			  void *buf, uint32_t len,
			  nvme_cb_fn_t cb_fn, void *cb_arg);

/**
 * \brief Process any outstanding completions for I/O submitted on the current thread.
 *
 * This will only process completions for I/O that were submitted on the same thread
 * that this function is called from. This call is also non-blocking, i.e. it only
 * processes completions that are ready at the time of this function call. It does not
 * wait for outstanding commands to finish.
 *
 * \param max_completions Limit the number of completions to be processed in one call, or 0
 * for unlimited.
 *
 * \return Number of completions processed (may be 0) or negative on error.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
int32_t nvme_ctrlr_process_io_completions(struct nvme_controller *ctrlr, uint32_t max_completions);

/**
 * \brief Send the given admin command to the NVMe controller.
 *
 * This is a low level interface for submitting admin commands directly. Prefer
 * the nvme_ctrlr_cmd_* functions instead. The validity of the command will
 * not be checked!
 *
 * When constructing the nvme_command it is not necessary to fill out the PRP
 * list/SGL or the CID. The driver will handle both of those for you.
 *
 * This function is thread safe and can be called at any point after
 * \ref nvme_attach().
 *
 * Call \ref nvme_ctrlr_process_admin_completions() to poll for completion
 * of commands submitted through this function.
 */
int nvme_ctrlr_cmd_admin_raw(struct nvme_controller *ctrlr,
			     struct nvme_command *cmd,
			     void *buf, uint32_t len,
			     nvme_cb_fn_t cb_fn, void *cb_arg);

/**
 * \brief Process any outstanding completions for admin commands.
 *
 * This will process completions for admin commands submitted on any thread.
 *
 * This call is non-blocking, i.e. it only processes completions that are ready
 * at the time of this function call. It does not wait for outstanding commands to
 * finish.
 *
 * \return Number of completions processed (may be 0) or negative on error.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 */
int32_t nvme_ctrlr_process_admin_completions(struct nvme_controller *ctrlr);


/** \brief Opaque handle to a namespace. Obtained by calling nvme_ctrlr_get_ns(). */
struct nvme_namespace;

/**
 * \brief Get a handle to a namespace for the given controller.
 *
 * Namespaces are numbered from 1 to the total number of namespaces. There will never
 * be any gaps in the numbering. The number of namespaces is obtained by calling
 * nvme_ctrlr_get_num_ns().
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
struct nvme_namespace *nvme_ctrlr_get_ns(struct nvme_controller *ctrlr, uint32_t ns_id);

/**
 * \brief Get a specific log page from the NVMe controller.
 *
 * \param log_page The log page identifier.
 * \param nsid Depending on the log page, this may be 0, a namespace identifier, or NVME_GLOBAL_NAMESPACE_TAG.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the log page has been retrieved.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be allocated for this request
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 * Call \ref nvme_ctrlr_process_admin_completions() to poll for completion
 * of commands submitted through this function.
 *
 * \sa nvme_ctrlr_is_log_page_supported()
 */
int nvme_ctrlr_cmd_get_log_page(struct nvme_controller *ctrlr,
				uint8_t log_page, uint32_t nsid,
				void *payload, uint32_t payload_size,
				nvme_cb_fn_t cb_fn, void *cb_arg);

/**
 * \brief Set specific feature for the given NVMe controller.
 *
 * \param feature The feature identifier.
 * \param cdw11 as defined by the specification for this command.
 * \param cdw12 as defined by the specification for this command.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the feature has been set.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be allocated for this request
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 * Call \ref nvme_ctrlr_process_admin_completions() to poll for completion
 * of commands submitted through this function.
 *
 * \sa nvme_ctrlr_cmd_set_feature()
 */
int nvme_ctrlr_cmd_set_feature(struct nvme_controller *ctrlr,
			       uint8_t feature, uint32_t cdw11, uint32_t cdw12,
			       void *payload, uint32_t payload_size,
			       nvme_cb_fn_t cb_fn, void *cb_arg);

/**
 * \brief Get specific feature from given NVMe controller.
 *
 * \param feature The feature identifier.
 * \param cdw11 as defined by the specification for this command.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the feature has been retrieved.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be allocated for this request
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 * Call \ref nvme_ctrlr_process_admin_completions() to poll for completion
 * of commands submitted through this function.
 *
 * \sa nvme_ctrlr_cmd_get_feature()
 */
int nvme_ctrlr_cmd_get_feature(struct nvme_controller *ctrlr,
			       uint8_t feature, uint32_t cdw11,
			       void *payload, uint32_t payload_size,
			       nvme_cb_fn_t cb_fn, void *cb_arg);

/**
 * \brief Get the identify namespace data as defined by the NVMe specification.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
const struct nvme_namespace_data *nvme_ns_get_data(struct nvme_namespace *ns);

/**
 * \brief Get the namespace id (index number) from the given namespace handle.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
uint32_t nvme_ns_get_id(struct nvme_namespace *ns);

/**
 * \brief Get the maximum transfer size, in bytes, for an I/O sent to the given namespace.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
uint32_t nvme_ns_get_max_io_xfer_size(struct nvme_namespace *ns);

/**
 * \brief Get the sector size, in bytes, of the given namespace.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
uint32_t nvme_ns_get_sector_size(struct nvme_namespace *ns);

/**
 * \brief Get the number of sectors for the given namespace.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
uint64_t nvme_ns_get_num_sectors(struct nvme_namespace *ns);

/**
 * \brief Get the size, in bytes, of the given namespace.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
uint64_t nvme_ns_get_size(struct nvme_namespace *ns);

/**
 * \brief Namespace command support flags.
 */
enum nvme_namespace_flags {
	NVME_NS_DEALLOCATE_SUPPORTED	= 0x1, /**< The deallocate command is supported */
	NVME_NS_FLUSH_SUPPORTED		= 0x2, /**< The flush command is supported */
	NVME_NS_RESERVATION_SUPPORTED	= 0x4, /**< The reservation command is supported */
};

/**
 * \brief Get the flags for the given namespace.
 *
 * See nvme_namespace_flags for the possible flags returned.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
uint32_t nvme_ns_get_flags(struct nvme_namespace *ns);

/**
 * Restart the SGL walk to the specified offset when the command has scattered payloads.
 *
 * The cb_arg parameter is the value passed to readv/writev.
 */
typedef void (*nvme_req_reset_sgl_fn_t)(void *cb_arg, uint32_t offset);

/**
 * Fill out *address and *length with the current SGL entry and advance to the next
 * entry for the next time the callback is invoked.
 *
 * The cb_arg parameter is the value passed to readv/writev.
 * The address parameter contains the physical address of this segment.
 * The length parameter contains the length of this physical segment.
 */
typedef int (*nvme_req_next_sge_fn_t)(void *cb_arg, uint64_t *address, uint32_t *length);

/**
 * \brief Submits a write I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the write I/O
 * \param payload virtual address pointer to the data payload
 * \param lba starting LBA to write the data
 * \param lba_count length (in sectors) for the write operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the NVME_IO_FLAGS_* entries
 * 			in spdk/nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 */
int nvme_ns_cmd_write(struct nvme_namespace *ns, void *payload,
		      uint64_t lba, uint32_t lba_count, nvme_cb_fn_t cb_fn,
		      void *cb_arg, uint32_t io_flags);

/**
 * \brief Submits a write I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the write I/O
 * \param lba starting LBA to write the data
 * \param lba_count length (in sectors) for the write operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined in nvme_spec.h, for this I/O
 * \param reset_sgl_fn callback function to reset scattered payload
 * \param next_sge_fn callback function to iterate each scattered
 * payload memory segment
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 */
int nvme_ns_cmd_writev(struct nvme_namespace *ns, uint64_t lba, uint32_t lba_count,
		       nvme_cb_fn_t cb_fn, void *cb_arg, uint32_t io_flags,
		       nvme_req_reset_sgl_fn_t reset_sgl_fn,
		       nvme_req_next_sge_fn_t next_sge_fn);

/**
 * \brief Submits a read I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the read I/O
 * \param payload virtual address pointer to the data payload
 * \param lba starting LBA to read the data
 * \param lba_count length (in sectors) for the read operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined in nvme_spec.h, for this I/O
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 */
int nvme_ns_cmd_read(struct nvme_namespace *ns, void *payload,
		     uint64_t lba, uint32_t lba_count, nvme_cb_fn_t cb_fn,
		     void *cb_arg, uint32_t io_flags);

/**
 * \brief Submits a read I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the read I/O
 * \param lba starting LBA to read the data
 * \param lba_count length (in sectors) for the read operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined in nvme_spec.h, for this I/O
 * \param reset_sgl_fn callback function to reset scattered payload
 * \param next_sge_fn callback function to iterate each scattered
 * payload memory segment
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 */
int nvme_ns_cmd_readv(struct nvme_namespace *ns, uint64_t lba, uint32_t lba_count,
		      nvme_cb_fn_t cb_fn, void *cb_arg, uint32_t io_flags,
		      nvme_req_reset_sgl_fn_t reset_sgl_fn,
		      nvme_req_next_sge_fn_t next_sge_fn);


/**
 * \brief Submits a deallocation request to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the deallocation request
 * \param payload virtual address pointer to the list of LBA ranges to
 *                deallocate
 * \param num_ranges number of ranges in the list pointed to by payload; must be
 *                between 1 and \ref NVME_DATASET_MANAGEMENT_MAX_RANGES, inclusive.
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 */
int nvme_ns_cmd_deallocate(struct nvme_namespace *ns, void *payload,
			   uint16_t num_ranges, nvme_cb_fn_t cb_fn,
			   void *cb_arg);

/**
 * \brief Submits a flush request to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the flush request
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 */
int nvme_ns_cmd_flush(struct nvme_namespace *ns, nvme_cb_fn_t cb_fn,
		      void *cb_arg);

/**
 * \brief Submits a reservation register to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the reservation register request
 * \param payload virtual address pointer to the reservation register data
 * \param ignore_key '1' the current reservation key check is disabled
 * \param action specifies the registration action
 * \param cptpl change the Persist Through Power Loss state
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 */
int nvme_ns_cmd_reservation_register(struct nvme_namespace *ns,
				     struct nvme_reservation_register_data *payload,
				     bool ignore_key,
				     enum nvme_reservation_register_action action,
				     enum nvme_reservation_register_cptpl cptpl,
				     nvme_cb_fn_t cb_fn, void *cb_arg);

/**
 * \brief Submits a reservation release to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the reservation release request
 * \param payload virtual address pointer to current reservation key
 * \param ignore_key '1' the current reservation key check is disabled
 * \param action specifies the reservation release action
 * \param type reservation type for the namespace
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 */
int nvme_ns_cmd_reservation_release(struct nvme_namespace *ns,
				    struct nvme_reservation_key_data *payload,
				    bool ignore_key,
				    enum nvme_reservation_release_action action,
				    enum nvme_reservation_type type,
				    nvme_cb_fn_t cb_fn, void *cb_arg);

/**
 * \brief Submits a reservation acquire to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the reservation acquire request
 * \param payload virtual address pointer to reservation acquire data
 * \param ignore_key '1' the current reservation key check is disabled
 * \param action specifies the reservation acquire action
 * \param type reservation type for the namespace
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 */
int nvme_ns_cmd_reservation_acquire(struct nvme_namespace *ns,
				    struct nvme_reservation_acquire_data *payload,
				    bool ignore_key,
				    enum nvme_reservation_acquire_action action,
				    enum nvme_reservation_type type,
				    nvme_cb_fn_t cb_fn, void *cb_arg);

/**
 * \brief Submits a reservation report to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the reservation report request
 * \param payload virtual address pointer for reservation status data
 * \param len length bytes for reservation status data structure
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 */
int nvme_ns_cmd_reservation_report(struct nvme_namespace *ns, void *payload,
				   uint32_t len, nvme_cb_fn_t cb_fn, void *cb_arg);

/**
 * \brief Get the size, in bytes, of an nvme_request.
 *
 * This is the size of the request objects that need to be allocated by the
 * nvme_alloc_request macro in nvme_impl.h
 *
 * This function is thread safe and can be called at any time.
 *
 */
size_t nvme_request_size(void);

int nvme_register_io_thread(void);
void nvme_unregister_io_thread(void);

#ifdef __cplusplus
}
#endif

#endif
