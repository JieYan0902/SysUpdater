/*
 * updater.h
 *
 *  Created on: Apr 12, 2017
 *      Author: airkiller1
 */

#ifndef UPDATER_H_
#define UPDATER_H_

#include <stdint.h>

#define MAX_PARAM_MQ_LEN 256

typedef enum
{
	UPDATER_APP = 0,
	UPDATER_KERNEL ,
	UPDATER_UBOOT ,
	UPDATER_STATUS ,
	UPDATER_PENDIND = -1
}updater_type;
typedef enum
{
	UPDATER_SUCCESS = 0,
	UPDATER_INITIALIZING,
	UPDATER_INITIALIZED,
	UPDATER_NOT_START,
	UPDATER_RUNNING,
	UPDATER_FAILED_SYS = -1,
	UPDATER_FAILED_SRC_CHECKSUM = -2,
	UPDATER_FAILED_DST_CHECKSUM = -3,
	UPDATER_FAILED_FORMAT_NOT_SUPPORTED = -4,
	UPDATER_FAILED_SINGLE_FILE_IN_PACKAGE = -5,
	UPDATER_FAILED_NO_CHECKSUM_FILE = -6,
	UPDATER_FAILED_UNEQUAL_IN_LENGTH = -7,
	UPDATER_FAILED_TIMEOUT = -8,
	UPDATER_FAILED_SHELL = -9,
}updater_status;
typedef enum
{
	STATE_RESP_BRIEF = 0,
	STATE_RESP_VERBOSE = 1
}updater_status_resp_type;
typedef enum
{
	OPT_SUCCESS = 0,
	OPT_FAILED_ZERO_LENGTH = -1,
	OPT_FAILED_JSON_FORMAT = -2,
	OPT_FAILED_INVALID_TYPE = -3,
	OPT_FAILED_INVALID_PRIO = -4,
	OPT_FAILED_IN_OPERANT = -5,
	OPT_FAILED_CMD_FORMAT = -6,
	OPT_FAILED_FILE_NONEXISTENT = -7,
	OPT_FAILED_THREAD_CONFIG = -8,
	OPT_FAILED_THREAD_START = -9,
	OPT_FAILED_PROCESS_START = -10,
}updater_response;

//cmai message type
typedef struct msg_params{

    /*valid data len in data[]*/
    uint32_t data_len;

    uint32_t src_id;

    uint32_t dst_id;

    uint32_t param_id;

    uint32_t wr_flg;

    /*buffer to param value*/
    char data[MAX_PARAM_MQ_LEN];
}msg_params;
#endif /* UPDATER_H_ */
