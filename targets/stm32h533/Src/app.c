#include "app.h"
#include "usb.h"
#include "main.h"
#include "utils.h"
#include <stdbool.h>
// #include "ctap_crypto_software.h"
// #include "ctap_memory_storage.h"
#include "flash_storage.h"
#include "hw_crypto.h"
#include "app_test.h"

// static uint8_t memory[16 * 1024];
// static ctap_memory_storage_context_t app_storage_ctx = {
// 	.memory_size = sizeof(memory),
// 	.memory = memory,
// };
// const ctap_storage_t app_storage = CTAP_MEMORY_STORAGE_CONST_INIT(&app_storage_ctx);

static stm32h533_flash_storage_context_t app_storage_ctx;
const ctap_storage_t app_storage = STM32H533_FLASH_STORAGE_CONST_INIT(&app_storage_ctx);

// static ctap_software_crypto_context_t app_crypto_ctx;
// const ctap_crypto_t app_sw_crypto = CTAP_SOFTWARE_CRYPTO_CONST_INIT(&app_crypto_ctx);

static stm32h533_crypto_context_t app_hw_crypto_ctx;
const ctap_crypto_t app_hw_crypto = STM32H533_CRYPTO_CONST_INIT(&app_hw_crypto_ctx);

ctaphid_state_t app_ctaphid;
static const uint8_t app_ctaphid_capabilities =
	CTAPHID_CAPABILITY_WINK | CTAPHID_CAPABILITY_CBOR | CTAPHID_CAPABILITY_NMSG;
static const uint8_t app_ctaphid_version_major = 1;
static const uint8_t app_ctaphid_version_minor = 0;
static const uint8_t app_ctaphid_version_build = 0;
// We could use a smaller value than CTAPHID_MAX_PAYLOAD_LENGTH
// since our CTAP layer does not return such large responses.
// We could compute theoretical maximum by examining the worst case (biggest size response)
// that could be generated by current CTAP implementation.
static uint8_t app_ctaphid_cbor_response_buffer[CTAPHID_MAX_PAYLOAD_LENGTH];
ctap_response_t app_ctap_response = {
	// The CTAP layer gracefully handles the case when the response does not fit into data_max_size bytes
	// by returning a CTAP1_ERR_OTHER error.
	.data_max_size = sizeof(app_ctaphid_cbor_response_buffer) - 1,
	.data = &app_ctaphid_cbor_response_buffer[1]
};
ctap_state_t app_ctap = CTAP_STATE_CONST_INIT(&app_hw_crypto, &app_storage);


static ctap_keepalive_status_t app_ctap_last_status = CTAP_STATUS_PROCESSING;
static uint32_t app_ctap_last_status_message_timestamp = 0;

static inline void app_ctap_reset_keepalive(void) {
	app_ctap_last_status = CTAP_STATUS_PROCESSING;
	app_ctap_last_status_message_timestamp = 0;
}

static void app_ctaphid_send_keepalive(ctap_keepalive_status_t status) {
	// debug_log("sending CTAPHID_KEEPALIVE" nl);
	assert(ctaphid_has_complete_message_ready(&app_ctaphid));
	const ctaphid_channel_buffer_t *message = &app_ctaphid.buffer;
	ctaphid_packet_t res;
	ctaphid_create_init_packet(&res, message->cid, CTAPHID_KEEPALIVE, 1);
	res.pkt.init.payload[0] = status;
	app_hid_report_send_queue_add(&res, false);
	app_ctap_last_status_message_timestamp = HAL_GetTick();
}

static void app_ctap_send_keepalive_if_needed(ctap_keepalive_status_t current_status) {

	// send immediately whenever the status changes
	if (current_status != app_ctap_last_status) {
		app_ctap_last_status = current_status;
		app_ctaphid_send_keepalive(current_status);
		return;
	}

	// but at least every 100ms
	uint32_t elapsed_since_last_keepalive = HAL_GetTick() - app_ctap_last_status_message_timestamp;
	// use a smaller value here to guarantee that the keepalive messages are sent frequently enough
	// even if ctap_send_keepalive_if_needed() is sometimes invoked late
	if (elapsed_since_last_keepalive > 80) {
		app_ctaphid_send_keepalive(current_status);
	}

}

// This function might be invoked anytime by the CTAP layer while in ctap_request().
void ctap_send_keepalive_if_needed(ctap_keepalive_status_t current_status) {
	app_ctap_send_keepalive_if_needed(current_status);
	app_hid_task();
	app_debug_task();
}

// This function might be invoked anytime by the CTAP layer while in ctap_request().
ctap_user_presence_result_t ctap_wait_for_user_presence(void) {

	info_log(yellow("waiting for user presence (press the ") cyan("BLUE") yellow(" button) ...") nl);
	app_ctap_send_keepalive_if_needed(CTAP_STATUS_UPNEEDED);

	Status_LED_Set_Mode(STATUS_LED_MODE_BLINKING_NORMAL);

	const uint32_t timeout_ms = 30 * 1000; // 30 seconds
	uint32_t start_timestamp = HAL_GetTick();

	if (BspButtonState == BUTTON_PRESSED) {
		BspButtonState = BUTTON_RELEASED;
	}

	while (true) {

		app_hid_task();
		app_debug_task();

		if (app_ctaphid.buffer.cancel) {
			info_log(yellow("ctap_wait_for_user_presence: ") red("got CANCEL via CTAPHID") nl);
			Status_LED_Set_Mode(STATUS_LED_MODE_ON);
			return CTAP_UP_RESULT_CANCEL;
		}

		uint32_t elapsed_ms = HAL_GetTick() - start_timestamp;
		if (elapsed_ms > timeout_ms) {
			info_log(yellow("ctap_wait_for_user_presence: ") red("TIMEOUT") nl);
			Status_LED_Set_Mode(STATUS_LED_MODE_ON);
			return CTAP_UP_RESULT_TIMEOUT;
		}

#ifdef LIONKEY_DEVELOPMENT_AUTO_USER_PRESENCE
		if (elapsed_ms > LIONKEY_DEVELOPMENT_AUTO_USER_PRESENCE) {
			info_log(
				yellow("ctap_wait_for_user_presence: ") green("AUTO_ALLOW after %d ms") nl,
				LIONKEY_DEVELOPMENT_AUTO_USER_PRESENCE
			);
			return CTAP_UP_RESULT_ALLOW;
		}
#endif

		if (BspButtonState == BUTTON_PRESSED) {
			BspButtonState = BUTTON_RELEASED;
			info_log(yellow("ctap_wait_for_user_presence: ") green("ALLOW") nl);
			Status_LED_Set_Mode(STATUS_LED_MODE_ON);
			return CTAP_UP_RESULT_ALLOW;
		}

		app_ctap_send_keepalive_if_needed(CTAP_STATUS_UPNEEDED);

	}

}

uint32_t ctap_get_current_time(void) {
	return HAL_GetTick();
}

static void handle_packet_using_send_or_queue_ctaphid_packet(const ctaphid_packet_t *packet, void *ctx) {
	UNUSED(ctx);
	app_hid_report_send_queue_add(packet, true);
}

static void app_init(void) {

	BSP_LED_On(LED_GREEN);

	const uint32_t t1 = HAL_GetTick();
	ctaphid_init(&app_ctaphid);
	if (app_storage.init(&app_storage) != CTAP_STORAGE_OK) {
		Error_Handler();
	}
	// if (app_sw_crypto.init(&app_sw_crypto, 0) != CTAP_CRYPTO_OK) {
	// 	Error_Handler();
	// }
	if (app_hw_crypto.init(&app_hw_crypto, 0) != CTAP_CRYPTO_OK) {
		Error_Handler();
	}
	ctap_init(&app_ctap);
	const uint32_t t2 = HAL_GetTick();

	info_log("init done in %" PRId32 " ms" nl, t2 - t1);

}

/**
 * Before processing the new CTAPHID request or turning a CTAP_CBOR response to CTAPHID packets,
 * call this function to ensure that the CTAPHID sending queue is completely empty
 * (i.e., the response to the previous message and any "immediate" single-packet responses generated
 * by app_handle_incoming_hid_packet() or app_ctaphid_send_keepalive() have been completely sent).
 *
 * Rationale:
 *   While processing the message, the sending might not progress much (because app_hid_task()
 *   is only invoked in the app_run() while loop and as a part of the ctap_wait_for_user_presence()).
 *   If we received a request (command) that resulted in a maximum-length long response
 *   while the sending queue had already been partially full, we could not add the response to the queue.
 *
 * Note the CTAPHID layer (ctaphid_process_packet) will correctly reject any new messages
 * with the CTAP1_ERR_CHANNEL_BUSY error code until the message is cleared
 * by calling ctaphid_reset_to_idle() (usually after the message is processed)
 * (see the CTAPHID_RESULT_ERROR case in app_handle_incoming_hid_packet()).
 */
static inline void app_wait_for_empty_hid_send_queue(void) {
	while (!app_hid_report_send_queue_is_empty()) {
		app_hid_task();
		app_debug_task();
	}
}

static inline void app_ctaphid_task(void) {

	if (!ctaphid_has_complete_message_ready(&app_ctaphid)) {
		return;
	}

	const ctaphid_channel_buffer_t *message = &app_ctaphid.buffer;
	const uint8_t cmd = message->cmd;

	debug_log(
		nl nl "app_run: " green("ctaphid message ready")
		" cid=0x%08" PRIx32
		" cmd=0x%02" wPRIx8
		" payload_length=%" PRIu16
		nl,
		message->cid, ctaphid_get_cmd_number_per_spec(cmd), message->payload_length
	);

	app_wait_for_empty_hid_send_queue();

	ctaphid_packet_t res;

	if (cmd == CTAPHID_PING) {
		debug_log(cyan("CTAPHID_PING") nl);
		ctaphid_message_to_packets(
			message->cid,
			CTAPHID_PING,
			message->payload_length,
			message->payload,
			handle_packet_using_send_or_queue_ctaphid_packet,
			NULL
		);
		ctaphid_reset_to_idle(&app_ctaphid);
		return;
	}

	if (cmd == CTAPHID_WINK) {
		debug_log(cyan("CTAPHID_WINK") nl);
		if (message->payload_length != 0) {
			info_log(red("error: invalid payload length 0 for CTAPHID_WINK message") nl);
			ctaphid_create_error_packet(&res, message->cid, CTAP1_ERR_INVALID_LENGTH);
			app_hid_report_send_queue_add(&res, true);
			ctaphid_reset_to_idle(&app_ctaphid);
			return;
		}
		// TODO: Do a LED blinking sequence to provide a "visual identification" of the authenticator.
		ctaphid_create_init_packet(&res, message->cid, CTAPHID_WINK, 0);
		app_hid_report_send_queue_add(&res, true);
		ctaphid_reset_to_idle(&app_ctaphid);
		return;
	}

	if (cmd == CTAPHID_CBOR) {
		debug_log(cyan("CTAPHID_CBOR") nl);
		if (message->payload_length == 0) {
			info_log(red("error: invalid payload length 0 for CTAPHID_CBOR message") nl);
			ctaphid_create_error_packet(&res, message->cid, CTAP1_ERR_INVALID_LENGTH);
			app_hid_report_send_queue_add(&res, true);
			ctaphid_reset_to_idle(&app_ctaphid);
			return;
		}
		assert(message->payload_length >= 1);
		app_ctap_reset_keepalive();
		app_ctaphid_cbor_response_buffer[0] = ctap_request(
			&app_ctap,
			message->payload[0],
			message->payload_length - 1,
			&message->payload[1],
			&app_ctap_response
		);
		// new HID reports (CTAPHID packets) might have been generated while in ctap_request():
		// ctap_request()
		//   -> (arbitrarily)
		//        ctap_send_keepalive_if_needed() (might also receive new CTAPHID packet)
		//   –> (when user presence is needed)
		//        ctap_wait_for_user_presence()
		//          -> receive and process new CTAPHID packet (if any)
		//          -> (periodically while waiting) ctap_send_keepalive_if_needed()
		app_wait_for_empty_hid_send_queue();
		ctaphid_message_to_packets(
			message->cid,
			CTAPHID_CBOR,
			1 + app_ctap_response.length,
			app_ctaphid_cbor_response_buffer,
			handle_packet_using_send_or_queue_ctaphid_packet,
			NULL
		);
		ctaphid_reset_to_idle(&app_ctaphid);
		return;
	}

	error_log(
		red("unsupported ctaphid command 0x%02" wPRIx8) nl,
		ctaphid_get_cmd_number_per_spec(cmd)
	);
	ctaphid_create_error_packet(&res, message->cid, CTAP1_ERR_INVALID_COMMAND);
	app_hid_report_send_queue_add(&res, true);
	ctaphid_reset_to_idle(&app_ctaphid);

}

noreturn void app_run(void) {

	info_log(nl nl cyan("app_run") nl);

	app_init();

	usb_init();

	while (true) {

		app_hid_task();

		app_debug_task();

		app_ctaphid_task();

	}

}

void app_handle_incoming_hid_packet(const ctaphid_packet_t *packet) {

	debug_log(nl);

	uint8_t error_code;
	ctaphid_packet_t res;
	uint32_t new_channel_id;

	const uint32_t current_time = HAL_GetTick();

	// first check for an incomplete message timeout and handle it if necessary
	if (ctaphid_has_incomplete_message_timeout(&app_ctaphid, current_time)) {
		debug_log(
			red("app_handle_incoming_hid_packet: incomplete message timeout on cid=0x%08" PRIx32) nl,
			app_ctaphid.buffer.cid
		);
		ctaphid_create_error_packet(&res, app_ctaphid.buffer.cid, CTAP1_ERR_TIMEOUT);
		ctaphid_reset_to_idle(&app_ctaphid);
	}

	// then continue with the processing of the just-received packet
	ctaphid_process_packet_result_t result = ctaphid_process_packet(
		&app_ctaphid,
		packet,
		current_time,
		&error_code
	);

	debug_log(
		"app_handle_incoming_hid_packet %s (%d)" nl "  ",
		lion_enum_str(ctaphid_process_packet_result, result),
		result
	);
	dump_hex((const uint8_t *) packet, sizeof(ctaphid_packet_t));

	switch (result) {

		case CTAPHID_RESULT_ERROR:
			ctaphid_create_error_packet(&res, packet->cid, error_code);
			app_hid_report_send_queue_add(&res, false);
			break;

		case CTAPHID_RESULT_ALLOCATE_CHANNEL:

			new_channel_id = ctaphid_allocate_channel(&app_ctaphid);

			if (new_channel_id == 0) {
				error_log(red(
					"app_handle_incoming_hid_packet: CTAPHID_RESULT_ALLOCATE_CHANNEL error no channel IDs left"
				) nl);
				ctaphid_create_error_packet(&res, packet->cid, error_code);
				app_hid_report_send_queue_add(&res, false);
				break;
			}

			ctaphid_create_ctaphid_init_response_packet(
				&res,
				packet->pkt.init.payload,
				CTAPHID_BROADCAST_CID,
				app_ctaphid.highest_allocated_cid,
				app_ctaphid_version_major,
				app_ctaphid_version_minor,
				app_ctaphid_version_build,
				app_ctaphid_capabilities
			);
			app_hid_report_send_queue_add(&res, false);
			break;

		case CTAPHID_RESULT_DISCARD_INCOMPLETE_MESSAGE:
			// Note:
			//   The actual discarding (if there was any) is already finished at this point,
			//   as it is done in the ctaphid_process_packet.
			//   Our only task here is to send the response.
			//   11.2.9.1.3. CTAPHID_INIT (0x06)
			//     https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-errata-20220621.html#usb-hid-init
			//     ... The device then responds with the CID of the channel it received the INIT on,
			//         using that channel.
			ctaphid_create_ctaphid_init_response_packet(
				&res,
				packet->pkt.init.payload,
				packet->cid,
				app_ctaphid.highest_allocated_cid,
				app_ctaphid_version_major,
				app_ctaphid_version_minor,
				app_ctaphid_version_build,
				app_ctaphid_capabilities
			);
			app_hid_report_send_queue_add(&res, false);
			break;

		case CTAPHID_RESULT_IGNORED:
			// nothing to do AT ALL
		case CTAPHID_RESULT_BUFFERING:
			// nothing to do AT ALL
		case CTAPHID_RESULT_CANCEL:
			// nothing to do HERE
			//
			// ctaphid_process_packet() sets the cancel flag (ctaphid_state_t.buffer.cancel)
			// on the buffer (which contains a complete CTAPHID_CBOR request).
			// If app_handle_incoming_hid_packet() was invoked during waiting for user presence,
			// the cancellation will be handled once tud_task() returns there.
		case CTAPHID_RESULT_MESSAGE:
			// nothing to do HERE
			//
			// To avoid nested invocations of tud_task() (which is probably not reentrant)
			// and unnecessary deep stack nesting, we leave the handling of this case to app_run.
			//
			// Here is an overview of how it all works together:
			//
			// app_run() {
			//    while(true) {
			//
			//        app_hid_task() -> tud_task() <- TinyUSB device "task"
			//            TinyUSB invokes tud_hid_set_report_cb() if a HID report was received from the host
			//            tud_hid_set_report_cb() invokes app_handle_incoming_hid_packet()
			//                app_handle_incoming_hid_packet()
			//                    processes the HID report by invoking ctaphid_process_packet()
			//                    and immediately handles some of the ctaphid_process_packet_result_t values
			//                    but leaves CTAPHID_RESULT_MESSAGE to app_run
			//
			//        if (ctaphid_has_complete_message_ready(&app_ctaphid)) {
			//            ... CTAPHID_RESULT_MESSAGE is handled here ...
			//            Note that handling CTAPHID_CBOR message (a CTAP request) might involve invoking tud_task()
			//            (and therefore app_handle_incoming_hid_packet()) during waiting for user presence
			//            because we still need to process incoming HID packets and respond to some of them,
			//            even while waiting for the user, resp. processing a CTAP request).
			//        }
			//    }
			// }
			//
			break;

	}

}
