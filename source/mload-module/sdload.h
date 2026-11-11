/*
	d2x cIOS: load extra modules from the SD card when the cIOS starts.

	MLOAD reads sd:/d2x/mload.txt, checks each module named there
	(sd:/d2x/mload/NAME) and starts it in its own thread. docs/mload.md has
	the design, the limits and the security notes.

	The list parser, the name check, the CRC and the ELF validator are plain
	functions on byte buffers; tests/host/mloadsd tests them, and the whole
	state machine, on the PC.
*/

#ifndef _SDLOAD_H_
#define _SDLOAD_H_

#include "types.h"

/* Files on the SD card */
#define SDL_LIST_PATH		"fat/d2x/mload.txt"
#define SDL_DIR_PATH		"fat/d2x/mload/"

/* Limits */
#define SDL_MAX_MODULES		8
#define SDL_NAME_MAX		31	/* characters; the IOS path stays under 64 bytes */
#define SDL_LIST_MAX		2048	/* bytes of the list that are read */
#define SDL_DEADLINE_MS		5000	/* from the start to the last step */
#define SDL_TICK_US		20000	/* the timer's period */

/* Message the timer sends to MLOAD's queue (outside every mapped address,
   so it cannot be an IPC message pointer) */
#define SDL_COOKIE		0x53444C44

/* States */
#define SDL_ST_IDLE		0
#define SDL_ST_WAIT_FAT		1
#define SDL_ST_MOUNT		2
#define SDL_ST_LIST		3
#define SDL_ST_LOAD		4
#define SDL_ST_DONE		5

/* Overall errors (report.error); 0 is none */
#define SDL_E_NOFAT		-1	/* the FAT module did not answer in time */
#define SDL_E_MOUNT		-2	/* no card, or no FAT volume on it */
#define SDL_E_NOLIST		-3	/* no sd:/d2x/mload.txt */
#define SDL_E_LISTIO		-4	/* the list could not be read */
#define SDL_E_TIMEOUT		-5	/* the 5 s budget ran out */
#define SDL_E_TITLE		-6	/* a title is running: never load then */
#define SDL_E_TIMER		-7	/* no timer */

/* Per module results (entry.result); 0 is success */
#define SDL_R_NAME		-10	/* not a plain name, or a bad crc32= */
#define SDL_R_OPEN		-11
#define SDL_R_READ		-12
#define SDL_R_ELF		-13	/* not a module ELF, or truncated */
#define SDL_R_RANGE		-14	/* outside the area, over another module, bad priority */
#define SDL_R_CRC		-15
#define SDL_R_THREAD		-16
#define SDL_R_LIMIT		-17	/* over SDL_MAX_MODULES (counted in skipped, not listed) */
#define SDL_R_NOTRUN		-18	/* never tried: the run ended first */

/* The report /dev/mload returns for MLOAD_GET_SD_REPORT. All fields are
   32 bits, big endian on the Wii. */
#define SDL_REPORT_MAGIC	0x53444D4C	/* 'SDML' */
#define SDL_REPORT_VERSION	1

typedef struct {
	char name[SDL_NAME_MAX + 1];	/* NUL terminated */
	s32  result;			/* 0, or SDL_R_* */
	s32  detail;			/* the thread id on success, the raw code on failure */
	u32  size;			/* the file's size in bytes, when it was read */
} sdl_entry;

typedef struct {
	u32 magic;			/* SDL_REPORT_MAGIC */
	u32 version;			/* SDL_REPORT_VERSION */
	u32 state;			/* SDL_ST_* */
	s32 error;			/* SDL_E_*, 0 for none */
	u32 count;			/* entries below that are filled in */
	u32 loaded;			/* modules started */
	u32 skipped;			/* list lines not started, extras included */
	u32 elapsed_ms;			/* from the start to now, or to the end */
	sdl_entry entries[SDL_MAX_MODULES];
} sdl_report;

/* Prototypes */
s32  SdLoad_Start(s32 queue);	/* create the timer; its id, or a negative error */
void SdLoad_Tick(void);		/* one step, on the timer's message */
s32  SdLoad_GetReport(void *out, u32 len);	/* 0, or IPC_EINVAL when len is short */

#endif
