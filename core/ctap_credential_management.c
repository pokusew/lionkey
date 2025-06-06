#include "ctap.h"

static inline uint8_t verify_credential_management_params(
	ctap_state_t *state,
	const CTAP_credentialManagement *params,
	ctap_pin_protocol_t *pin_protocol
) {
	uint8_t verify_ctx[pin_protocol->verify_get_context_size(pin_protocol)];
	if (pin_protocol->verify_init_with_pin_uv_auth_token(
		pin_protocol,
		&verify_ctx,
		&state->pin_uv_auth_token_state
	) != 0) {
		return CTAP2_ERR_PIN_AUTH_INVALID;
	}
	pin_protocol->verify_update(
		pin_protocol,
		&verify_ctx,
		/* message */ &params->subCommand, sizeof(params->subCommand)
	);
	if (params->subCommandParams.raw_size > 0) {
		pin_protocol->verify_update(
			pin_protocol,
			&verify_ctx,
			/* message */ params->subCommandParams.raw, params->subCommandParams.raw_size
		);
	}
	if (pin_protocol->verify_final(
		pin_protocol,
		&verify_ctx,
		/* signature */ params->pinUvAuthParam.data, params->pinUvAuthParam.size
	) != 0) {
		return CTAP2_ERR_PIN_AUTH_INVALID;
	}
	if (!ctap_pin_uv_auth_token_has_permissions(state, CTAP_clientPIN_pinUvAuthToken_permission_cm)) {
		debug_log(
			red("pinUvAuthToken does not have the cm permission:"
				" current=%" PRIu32 " required=%" PRIu32) nl,
			state->pin_uv_auth_token_state.permissions,
			(uint32_t) CTAP_clientPIN_pinUvAuthToken_permission_cm
		);
		return CTAP2_ERR_PIN_AUTH_INVALID;
	}
	return CTAP2_OK;
}

static uint8_t credential_management_get_creds_metadata(ctap_state_t *const state, CborEncoder *const encoder) {

	// the pinUvAuthToken used for getCredsMetadata must NOT have a permissions RP ID associated
	if (state->pin_uv_auth_token_state.rpId_set) {
		return CTAP2_ERR_PIN_AUTH_INVALID;
	}

	CborError err;
	CborEncoder map;

	cbor_encoding_check(cbor_encoder_create_map(encoder, &map, 2));
	cbor_encoding_check(cbor_encode_uint(
		&map, CTAP_credentialManagement_res_existingResidentCredentialsCount
	));
	cbor_encoding_check(cbor_encode_uint(
		&map, ctap_count_num_stored_discoverable_credentials(state->storage)
	));
	cbor_encoding_check(cbor_encode_uint(
		&map, CTAP_credentialManagement_res_maxPossibleRemainingResidentCredentialsCount
	));
	cbor_encoding_check(cbor_encode_uint(
		&map, ctap_get_num_max_possible_remaining_discoverable_credentials(state->storage)
	));
	cbor_encoding_check(cbor_encoder_close_container(encoder, &map));

	return CTAP2_OK;

}

static uint8_t encode_credential_management_enumerate_rps_response(
	CborEncoder *encoder,
	const CTAP_rpId_hash_ptr *rp_id,
	const size_t num_rps
) {

	uint8_t ret;
	CborError err;
	CborEncoder map;

	cbor_encoding_check(cbor_encoder_create_map(encoder, &map, num_rps != 0 ? 3 : 2));

	cbor_encoding_check(cbor_encode_uint(&map, CTAP_credentialManagement_res_rp));
	ctap_check(ctap_encode_rp_entity(&map, &rp_id->id));

	cbor_encoding_check(cbor_encode_uint(&map, CTAP_credentialManagement_res_rpIDHash));
	cbor_encoding_check(cbor_encode_byte_string(&map, rp_id->hash, CTAP_SHA256_HASH_SIZE));

	if (num_rps != 0) {
		cbor_encoding_check(cbor_encode_uint(&map, CTAP_credentialManagement_res_totalRPs));
		cbor_encoding_check(cbor_encode_uint(&map, num_rps));
	}

	cbor_encoding_check(cbor_encoder_close_container(encoder, &map));

	return CTAP2_OK;

}

static uint8_t credential_management_enumerate_rps_begin(ctap_state_t *const state, CborEncoder *const encoder) {

	// the pinUvAuthToken used for enumerateRPsBegin must NOT have a permissions RP ID associated
	if (state->pin_uv_auth_token_state.rpId_set) {
		return CTAP2_ERR_PIN_AUTH_INVALID;
	}

	uint8_t ret;

	ctap_discard_stateful_command_state(state);
	ctap_cred_mgmt_enumerate_rps_state_t *enumerate_rps_state = &state->stateful_command_state.cred_mgmt_enumerate_rps;
	enumerate_rps_state->num_rps = 0;
	enumerate_rps_state->next_rp_idx = 0;
	const size_t max_num_rp_ids = sizeof(enumerate_rps_state->rp_ids) / sizeof(CTAP_rpId_hash_ptr);
	ctap_check(ctap_enumerate_rp_ids_of_discoverable_credentials(
		state->storage,
		enumerate_rps_state->rp_ids,
		&enumerate_rps_state->num_rps,
		max_num_rp_ids
	));

	if (enumerate_rps_state->num_rps == 0) {
		return CTAP2_ERR_NO_CREDENTIALS;
	}

	ctap_check(encode_credential_management_enumerate_rps_response(
		encoder,
		&enumerate_rps_state->rp_ids[enumerate_rps_state->next_rp_idx],
		enumerate_rps_state->num_rps
	));

	if (enumerate_rps_state->num_rps > 1) {
		state->stateful_command_state.active_cmd = CTAP_STATEFUL_CMD_CRED_MGMT_ENUMERATE_RPS;
		enumerate_rps_state->next_rp_idx++;
		ctap_update_stateful_command_timer(state);
	} else {
		// We could just leave the partial state in memory since it's not valid anyway
		// (because, at this point, stateful_command_state.active_cmd == CTAP_STATEFUL_CMD_NONE).
		// However, as a good practice, we want to avoid keeping any potentially sensitive state
		// in memory longer than necessary. To avoid unnecessary large memset()
		// in ctap_discard_stateful_command_state(), we manually clean up the partial state.
		assert(enumerate_rps_state->num_rps == 1);
		enumerate_rps_state->num_rps = 0;
		static_assert(
			sizeof(enumerate_rps_state->rp_ids[0]) == sizeof(CTAP_rpId_hash_ptr),
			"sizeof(enumerate_rps_state->rp_ids[0]) == sizeof(CTAP_rpId_hash_ptr)"
		);
		memset(
			&enumerate_rps_state->rp_ids[0],
			0,
			sizeof(enumerate_rps_state->rp_ids[0])
		);
	}

	return CTAP2_OK;

}

static uint8_t credential_management_enumerate_rps_get_next_rp(ctap_state_t *const state, CborEncoder *const encoder) {

	if (state->stateful_command_state.active_cmd != CTAP_STATEFUL_CMD_CRED_MGMT_ENUMERATE_RPS) {
		return CTAP2_ERR_NOT_ALLOWED;
	}

	ctap_cred_mgmt_enumerate_rps_state_t *enumerate_rps_state = &state->stateful_command_state.cred_mgmt_enumerate_rps;

	uint8_t ret;

	ctap_check(encode_credential_management_enumerate_rps_response(
		encoder,
		&enumerate_rps_state->rp_ids[enumerate_rps_state->next_rp_idx],
		0 // totalRPs (0x05) is omitted for the authenticatorCredentialManagement/enumerateRPsGetNextRP
	));

	ctap_update_stateful_command_timer(state);
	enumerate_rps_state->next_rp_idx++;
	if (enumerate_rps_state->next_rp_idx == enumerate_rps_state->num_rps) {
		ctap_discard_stateful_command_state(state);
	}

	return CTAP2_OK;

}

static uint8_t encode_credential_management_enumerate_credentials_response(
	CborEncoder *encoder,
	const ctap_credential_handle_t *credential,
	const uint8_t *credential_public_key,
	const size_t num_credentials
) {

	uint8_t ret;
	CborError err;
	CborEncoder map;

	cbor_encoding_check(cbor_encoder_create_map(
		encoder, &map, num_credentials != 0 ? 5 : 4
	));

	cbor_encoding_check(cbor_encode_uint(&map, CTAP_credentialManagement_res_user));
	CTAP_userEntity user;
	ctap_credential_get_user(credential, &user);
	ctap_check(ctap_encode_pub_key_cred_user_entity(&map, &user, true));

	cbor_encoding_check(cbor_encode_uint(&map, CTAP_credentialManagement_res_credentialID));
	ctap_string_t credential_id;
	ctap_credential_get_id(credential, &credential_id);
	ctap_check(ctap_encode_pub_key_cred_desc(&map, &credential_id));

	cbor_encoding_check(cbor_encode_uint(&map, CTAP_credentialManagement_res_publicKey));
	ctap_check(ctap_encode_public_key(&map, credential_public_key));

	if (num_credentials != 0) {
		cbor_encoding_check(cbor_encode_uint(&map, CTAP_credentialManagement_res_totalCredentials));
		cbor_encoding_check(cbor_encode_uint(&map, num_credentials));
	}

	cbor_encoding_check(cbor_encode_uint(&map, CTAP_credentialManagement_res_credProtect));
	cbor_encoding_check(cbor_encode_uint(&map, ctap_credential_get_cred_protect(credential)));

	cbor_encoding_check(cbor_encoder_close_container(encoder, &map));

	return CTAP2_OK;

}

static uint8_t credential_management_enumerate_credentials_begin(
	ctap_state_t *const state,
	const CTAP_credentialManagement *const cm,
	CborEncoder *const encoder
) {

	if ((
		!ctap_param_is_present(cm, CTAP_credentialManagement_subCommandParams)
		|| !ctap_param_is_present(&cm->subCommandParams, CTAP_credentialManagement_subCommandParams_rpIDHash)
	)) {
		return CTAP2_ERR_MISSING_PARAMETER;
	}

	// the pinUvAuthToken used for enumerateCredentialsBegin must
	// either NOT have a permissions RP ID associated,
	// or the associated permissions RP ID MUST match the RP ID of this request
	if ((
		state->pin_uv_auth_token_state.rpId_set
		&& (
			// the rpIDHash.size should already be checked by ctap_parse_credential_management()
			// (resp. parse_credential_management_subcommand_params())
			cm->subCommandParams.rpIDHash.size != CTAP_SHA256_HASH_SIZE
			|| memcmp(
				cm->subCommandParams.rpIDHash.data,
				state->pin_uv_auth_token_state.rpId_hash,
				CTAP_SHA256_HASH_SIZE
			) != 0
		)
	)) {
		debug_log(
			"credential_management_enumerate_credentials_begin: the pinUvAuthToken has a RP ID associated"
			"but it does not match the RP ID of this request" nl
		);
		return CTAP2_ERR_PIN_AUTH_INVALID;
	}

	uint8_t ret;

	ctap_discard_stateful_command_state(state);
	ctap_cred_mgmt_enumerate_credentials_state_t *enumerate_credentials_state =
		&state->stateful_command_state.cred_mgmt_enumerate_credentials;
	enumerate_credentials_state->num_credentials = 0;
	enumerate_credentials_state->next_credential_idx = 0;
	const size_t max_num_credentials =
		sizeof(enumerate_credentials_state->credentials) / sizeof(ctap_credential_handle_t);
	ctap_check(ctap_find_discoverable_credentials_by_rp_id(
		state->storage,
		NULL,
		cm->subCommandParams.rpIDHash.data,
		true,
		enumerate_credentials_state->credentials,
		&enumerate_credentials_state->num_credentials,
		max_num_credentials
	));

	if (enumerate_credentials_state->num_credentials == 0) {
		return CTAP2_ERR_NO_CREDENTIALS;
	}

	const ctap_credential_handle_t *credential = &enumerate_credentials_state->credentials[
		enumerate_credentials_state->next_credential_idx
	];
	uint8_t credential_public_key[64];
	ctap_check(ctap_credential_compute_public_key(
		state->crypto,
		credential,
		credential_public_key
	));
	ctap_check(encode_credential_management_enumerate_credentials_response(
		encoder,
		credential,
		credential_public_key,
		enumerate_credentials_state->num_credentials
	));

	if (enumerate_credentials_state->num_credentials > 1) {
		state->stateful_command_state.active_cmd = CTAP_STATEFUL_CMD_CRED_MGMT_ENUMERATE_CREDENTIALS;
		enumerate_credentials_state->next_credential_idx++;
		ctap_update_stateful_command_timer(state);
	} else {
		// We could just leave the partial state in memory since it's not valid anyway
		// (because, at this point, stateful_command_state.active_cmd == CTAP_STATEFUL_CMD_NONE).
		// However, as a good practice, we want to avoid keeping any potentially sensitive state
		// in memory longer than necessary. To avoid unnecessary large memset()
		// in ctap_discard_stateful_command_state(), we manually clean up the partial state.
		assert(enumerate_credentials_state->num_credentials == 1);
		enumerate_credentials_state->num_credentials = 0;
		static_assert(
			sizeof(enumerate_credentials_state->credentials[0]) == sizeof(ctap_credential_handle_t),
			"sizeof(enumerate_credentials_state->credentials[0]) == sizeof(ctap_credential_handle_t)"
		);
		memset(
			&enumerate_credentials_state->credentials[0],
			0,
			sizeof(enumerate_credentials_state->credentials[0])
		);
	}

	return CTAP2_OK;

}

static uint8_t credential_management_enumerate_credentials_get_next_credential(
	ctap_state_t *const state,
	CborEncoder *const encoder
) {

	if (state->stateful_command_state.active_cmd != CTAP_STATEFUL_CMD_CRED_MGMT_ENUMERATE_CREDENTIALS) {
		return CTAP2_ERR_NOT_ALLOWED;
	}

	ctap_cred_mgmt_enumerate_credentials_state_t *enumerate_credentials_state =
		&state->stateful_command_state.cred_mgmt_enumerate_credentials;

	uint8_t ret;

	const ctap_credential_handle_t *credential = &enumerate_credentials_state->credentials[
		enumerate_credentials_state->next_credential_idx
	];
	uint8_t credential_public_key[64];
	ctap_check(ctap_credential_compute_public_key(
		state->crypto,
		credential,
		credential_public_key
	));
	ctap_check(encode_credential_management_enumerate_credentials_response(
		encoder,
		credential,
		credential_public_key,
		0 // totalCredentials (0x09) is omitted for the authenticatorCredentialManagement/enumerateCredentialsGetNextCredential
	));

	ctap_update_stateful_command_timer(state);
	enumerate_credentials_state->next_credential_idx++;
	if (enumerate_credentials_state->next_credential_idx == enumerate_credentials_state->num_credentials) {
		ctap_discard_stateful_command_state(state);
	}

	return CTAP2_OK;

}

/**
 * Ensures that the current pinUvAuthToken can be used to perform an operation on a given credential.
 *
 * The pinUvAuthToken MUST either NOT have a permissions RP ID associated,
 * or the associated permissions RP ID MUST match the RP ID of the credential.
 *
 * @param state the current CTAP state
 * @param key the credential
 * @return a CTAP status code
 * @retval CTAP2_OK the current pinUvAuthToken can be used
 * @retval CTAP2_ERR_PIN_AUTH_INVALID the pinUvAuthToken has a RP ID associated,
 *                                    but it does not match the RP ID of the credential
 */
static uint8_t ensure_pin_uv_auth_token_can_be_used(
	const ctap_state_t *const state,
	const ctap_credential_handle_t *const credential
) {
	if ((
		state->pin_uv_auth_token_state.rpId_set
		&& !ctap_credential_matches_rp_id_hash(credential, state->pin_uv_auth_token_state.rpId_hash)
	)) {
		debug_log(
			"ensure_pin_uv_auth_token_can_be_used: the pinUvAuthToken has a RP ID associated"
			"but it does not match the RP ID of the credential" nl
		);
		return CTAP2_ERR_PIN_AUTH_INVALID;
	}
	return CTAP2_OK;
}

static uint8_t credential_management_delete_credential(
	ctap_state_t *const state,
	const CTAP_credentialManagement *const cm
) {

	uint8_t ret;

	ctap_discard_stateful_command_state(state);

	if ((
		!ctap_param_is_present(cm, CTAP_credentialManagement_subCommandParams)
		|| !ctap_param_is_present(&cm->subCommandParams, CTAP_credentialManagement_subCommandParams_credentialID)
	)) {
		return CTAP2_ERR_MISSING_PARAMETER;
	}

	// find credential
	ctap_credential_handle_t credential;
	if (!ctap_lookup_credential_by_desc(state->storage, &cm->subCommandParams.credentialID, &credential)) {
		return CTAP2_ERR_NO_CREDENTIALS;
	}

	ctap_check(ensure_pin_uv_auth_token_can_be_used(state, &credential));

	return ctap_delete_credential(state->storage, &credential);

}

static uint8_t credential_management_update_user_information(
	ctap_state_t *const state,
	const CTAP_credentialManagement *const cm
) {

	uint8_t ret;

	ctap_discard_stateful_command_state(state);

	if ((
		!ctap_param_is_present(cm, CTAP_credentialManagement_subCommandParams)
		|| !ctap_param_is_present(&cm->subCommandParams, CTAP_credentialManagement_subCommandParams_credentialID)
	)) {
		return CTAP2_ERR_MISSING_PARAMETER;
	}

	// find credential
	ctap_credential_handle_t credential;
	if (!ctap_lookup_credential_by_desc(state->storage, &cm->subCommandParams.credentialID, &credential)) {
		return CTAP2_ERR_NO_CREDENTIALS;
	}

	ctap_check(ensure_pin_uv_auth_token_can_be_used(state, &credential));

	const CTAP_userEntity *const updated_user = &cm->subCommandParams.user;

	ctap_check(ctap_credential_update_user_information(state->storage, &credential, updated_user));

	return CTAP2_OK;

}

uint8_t ctap_credential_management(ctap_state_t *const state, CborValue *const it, CborEncoder *const encoder) {

	uint8_t ret;

	CTAP_credentialManagement cm;
	ctap_check(ctap_parse_credential_management(it, &cm));

	// 6.8. authenticatorCredentialManagement (0x0A)
	// https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-errata-20220621.html#authenticatorCredentialManagement

	// common steps for all subcommands

	if (!ctap_param_is_present(&cm, CTAP_credentialManagement_subCommand)) {
		return CTAP2_ERR_MISSING_PARAMETER;
	}

	const bool pinUvAuthParamRequired =
		cm.subCommand != CTAP_credentialManagement_subCmd_enumerateRPsGetNextRP
		&& cm.subCommand != CTAP_credentialManagement_subCmd_enumerateCredentialsGetNextCredential;
	if (pinUvAuthParamRequired) {
		if (!ctap_param_is_present(&cm, CTAP_credentialManagement_pinUvAuthParam)) {
			return CTAP2_ERR_PUAT_REQUIRED;
		}
		if (!ctap_param_is_present(&cm, CTAP_credentialManagement_pinUvAuthProtocol)) {
			return CTAP2_ERR_MISSING_PARAMETER;
		}
		ctap_pin_protocol_t *pin_protocol;
		// if the given pinUvAuthProtocol is not supported, it returns CTAP1_ERR_INVALID_PARAMETER
		ctap_check(ctap_get_pin_protocol(state, cm.pinUvAuthProtocol, &pin_protocol));
		// Authenticator calls verify(pinUvAuthToken, subCommand || subCommandParams, pinUvAuthParam)
		ctap_check(verify_credential_management_params(state, &cm, pin_protocol));
	}

	switch (cm.subCommand) {

		case CTAP_credentialManagement_subCmd_getCredsMetadata:
			debug_log(magenta("CTAP_credentialManagement_subCmd_getCredsMetadata") nl);
			return credential_management_get_creds_metadata(state, encoder);

		case CTAP_credentialManagement_subCmd_enumerateRPsBegin:
			debug_log(magenta("CTAP_credentialManagement_subCmd_enumerateRPsBegin") nl);
			return credential_management_enumerate_rps_begin(state, encoder);

		case CTAP_credentialManagement_subCmd_enumerateRPsGetNextRP:
			debug_log(magenta("CTAP_credentialManagement_subCmd_enumerateRPsGetNextRP") nl);
			return credential_management_enumerate_rps_get_next_rp(state, encoder);

		case CTAP_credentialManagement_subCmd_enumerateCredentialsBegin:
			debug_log(magenta("CTAP_credentialManagement_subCmd_enumerateCredentialsBegin") nl);
			return credential_management_enumerate_credentials_begin(state, &cm, encoder);

		case CTAP_credentialManagement_subCmd_enumerateCredentialsGetNextCredential:
			debug_log(magenta("CTAP_credentialManagement_subCmd_enumerateCredentialsGetNextCredential") nl);
			return credential_management_enumerate_credentials_get_next_credential(state, encoder);

		case CTAP_credentialManagement_subCmd_deleteCredential:
			debug_log(magenta("CTAP_credentialManagement_subCmd_deleteCredential") nl);
			return credential_management_delete_credential(state, &cm);

		case CTAP_credentialManagement_subCmd_updateUserInformation:
			debug_log(magenta("CTAP_credentialManagement_subCmd_updateUserInformation") nl);
			return credential_management_update_user_information(state, &cm);

	}

	// default case (unknown or unsupported subcommand)
	return CTAP2_ERR_INVALID_SUBCOMMAND;

}
