/*
 * This file is part of testrunner-lite
 *
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
 * Contains changes by Wind River Systems, 2011-03-09
 *
 * Contact: Sampo Saaristo <sampo.saaristo@sofica.fi>
 *
 * Copyright (C) 2013 Jolla Ltd.
 * Contact: Jakub Adam <jakub.adam@jollamobile.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

/* ------------------------------------------------------------------------- */
/* INCLUDE FILES */
#include <dirent.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <libxml/tree.h>
#include <signal.h>
#include <sys/inotify.h>
#include <uuid/uuid.h>

#include "testrunnerlite.h"
#include "testdefinitionparser.h"
#include "testresultlogger.h"
#include "testfilters.h"
#include "testmeasurement.h"
#include "executor.h"
#include "remote_executor.h"
#include "manual_executor.h"
#include "utils.h"
#include "log.h"
#ifdef ENABLE_EVENTS
#include "event.h"
#endif

/* ------------------------------------------------------------------------- */
/* EXTERNAL DATA STRUCTURES */
/* None */

/* ------------------------------------------------------------------------- */
/* EXTERNAL GLOBAL VARIABLES */
extern char* optarg;

/* ------------------------------------------------------------------------- */
/* EXTERNAL FUNCTION PROTOTYPES */
/* None */

/* ------------------------------------------------------------------------- */
/* GLOBAL VARIABLES */
struct timeval created;
testrunner_lite_options opts;
char *global_failure = NULL;
int bail_out = 0;
/* ------------------------------------------------------------------------- */
/* CONSTANTS */
/* None */

/* ------------------------------------------------------------------------- */
/* MACROS */
/* None */

/* ------------------------------------------------------------------------- */
/* LOCAL GLOBAL VARIABLES */
LOCAL td_td    *current_td = NULL;    /* Test definition currently executed */
LOCAL td_suite *current_suite = NULL; /* Suite currently executed */
LOCAL td_set   *current_set = NULL;   /* Set currently executed */
LOCAL xmlChar  *cur_case_name = BAD_CAST"";   /* Name of the current case */ 
LOCAL int       cur_step_num;         /* Number of current step within case */

LOCAL int passcount = 0;
LOCAL int failcount = 0;
LOCAL int casecount = 0;
/* ------------------------------------------------------------------------- */
/* LOCAL CONSTANTS AND MACROS */
const char *TESTCASE_UUID_FILENAME = "testrunner-lite-testcase";
/* ------------------------------------------------------------------------- */
/* MODULE DATA STRUCTURES */
/* None */

/* ------------------------------------------------------------------------- */
/* LOCAL FUNCTION PROTOTYPES */
/* ------------------------------------------------------------------------- */
LOCAL void process_td(td_td *);
/* ------------------------------------------------------------------------- */
LOCAL void end_td();
/* ------------------------------------------------------------------------- */
LOCAL void process_hwiddetect();
/* ------------------------------------------------------------------------- */
LOCAL void process_suite(td_suite *);
/* ------------------------------------------------------------------------- */
LOCAL void end_suite ();
/* ------------------------------------------------------------------------- */
LOCAL void process_set(td_set *);
/* ------------------------------------------------------------------------- */
LOCAL int process_case (const void *, const void *);
/* ------------------------------------------------------------------------- */
LOCAL int case_result_fail (const void *, const void *);
/* ------------------------------------------------------------------------- */
LOCAL int process_get (const void *, const void *);
/* ------------------------------------------------------------------------- */
LOCAL int process_get_case (const void *, const void *);
/* ------------------------------------------------------------------------- */
LOCAL int step_execute (const void *, const void *);
/* ------------------------------------------------------------------------- */
LOCAL int prepost_steps_execute (const void *, const void *);
/* ------------------------------------------------------------------------- */
LOCAL int step_result_fail (const void *, const void *);
/* ------------------------------------------------------------------------- */
LOCAL int step_post_process (const void *, const void *);
/* ------------------------------------------------------------------------- */
#ifdef ENABLE_EVENTS
LOCAL int event_execute (const void *data, const void *user);
/* ------------------------------------------------------------------------- */
#endif
LOCAL void set_device_core_pattern (const char *);
/* ------------------------------------------------------------------------- */
LOCAL int fetch_rich_core_dumps (const char *, xmlHashTablePtr);
/* ------------------------------------------------------------------------- */

/* FORWARD DECLARATIONS */
/* None */

/* ------------------------------------------------------------------------- */
/* ==================== LOCAL FUNCTIONS ==================================== */
/* ------------------------------------------------------------------------- */

/**
 * @file a file name
 * @return the file name prepended with path to rich-core-dumper's output
 *         directory; has to be free()'d after use
 */
LOCAL char *path_in_core_dumps(const char *file)
{
	char *result =
		(char*) malloc(strlen(opts.rich_core_dumps) + strlen(file) + 1);
	sprintf(result, "%s%s", opts.rich_core_dumps, file);

	return result;
}

/**
 * Checks whether given crash report entry misses its telemetry URL. Should be
 * used as a scanner function argument to xmlHashScan().
 *
 * @param url a crash report URL on telemetry server
 * @param result pointer to int that is set to 1 when url is empty
 * @param unused
 */
LOCAL void check_empty_url (char *url, int *result, xmlChar *unused)
{
	if (strlen(url) == 0) {
		*result = 1;
	}
}

/**
 * @return 1 when any crash report in given hash table misses its telemetry URL,
 *         otherwise 0
 */
LOCAL int has_pending_core_uploads (xmlHashTablePtr crashes)
{
	int result = 0;

	xmlHashScan(crashes, (xmlHashScanner)check_empty_url, &result);

	return result;
}

/**
 * For crash logs given as keys in a hash table reads their corresponding
 * telemetry URLs from crash-reporter's upload log and stores them in the
 * hash table as values to their respective keys.
 *
 * @param crashes xmlHashTablePtr pre-filled with crash-log filenames
 * @return 0 if URLs for all crash reports were found, otherwise 1
 */
LOCAL int collect_urls_from_uploadlog (xmlHashTablePtr crashes)
{
	const char *UPLOADLOG_FILENAME = "uploadlog";
	char *uploadlog_path = path_in_core_dumps(UPLOADLOG_FILENAME);
	FILE *uploadlog = NULL;
	char line[256];

	uploadlog = fopen(uploadlog_path, "r");
	if (!uploadlog) {
		LOG_MSG(LOG_DEBUG, "Couldn't open crash-reporter upload log\n");
		goto out;
	}

	while (fgets(line, sizeof line, uploadlog)) {
		char *filename;
		char *url;
		char *stored_filename;
		size_t line_len = strlen(line);
		if (line_len == 0) {
			continue;
		}

		if (line[line_len - 1] == '\n') {
			line[line_len - 1] = '\0';
		}

		url = strrchr(line, ' ');
		if (!url) {
			continue;
		}
		filename = url;
		url += 1;

		/* Zero-out any spaces between core file name and telemetry URL.
		 * Then we'll be able to read both strings from line buffer. */
		while (filename >= line && *filename == ' ') {
			*filename = '\0';
		}
		filename = line;

		stored_filename = xmlHashLookup(crashes, (xmlChar*)filename);
		if (stored_filename && (strlen(stored_filename) == 0)) {
			// New upload detected
			xmlHashUpdateEntry(crashes, (xmlChar*)filename,
					strdup(url),
					(xmlHashDeallocator)xmlFree);
			LOG_MSG(LOG_DEBUG, "Telemetry URL for %s is %s\n",
					filename, url);
		}
	}

out:
	if (uploadlog)
		fclose(uploadlog);
	free(uploadlog_path);

	return has_pending_core_uploads(crashes);
}

/**
 * Downloads a crash report file into testrunner's destination directory, if
 * it wasn't already uploaded to telemetry server by crash-reporter. Should be
 * used as a scanner function argument to xmlHashScan().
 *
 * @param url a crash report URL on telemetry server
 * @param unused
 * @param filename crash report's file name
 */
LOCAL void fetch_leftover_report (xmlChar *url, void *unused, char *filename)
{
	td_file *file;

	if (xmlStrlen(url) > 0) {
		return;
	}

	file = (td_file *)malloc (sizeof (td_file));
	file->filename = (xmlChar*)path_in_core_dumps(filename);
	file->delete_after = 1;
	file->measurement = 0;
	file->series = 0;
	process_get (file, 0);

	td_file_delete(file);
}

/**
 * Into a hash table, stores file names of crash reports created during a run of
 * a test case specified by its uuid. After this function is run, given hash
 * table will contain pairs having crash report file name as a key and an empty
 * string as a value.
 *
 * @param uuid a UUID of a test case
 * @param crashes hash table where to store the crash report file names
 */
LOCAL void collect_crash_reports (const char *uuid, xmlHashTablePtr crashes)
{
	DIR *dir = opendir(opts.rich_core_dumps);
	struct dirent *entry;

	if (!dir) {
		LOG_MSG(LOG_ERR, "%s: Couldn't open core dump directory",
			PROGNAME);
		return;
	}

	while ((entry = readdir(dir))) {
		xmlChar *report_filename;
		char *marker_path;

		size_t marker_filename_len = strlen(entry->d_name);
		size_t report_filename_len = marker_filename_len - strlen(uuid);


		if (entry->d_type != DT_REG) {
			continue;
		}

		if (strcmp(entry->d_name + report_filename_len, uuid)) {
			/* Filename doesn't end with uuid, thus it's not a
			 * crash report marker file, skip. */
			continue;
		}

		// Get rid of a dot before uuid
		report_filename_len--;

		report_filename = xmlStrndup((xmlChar*)entry->d_name,
				report_filename_len);

		LOG_MSG (LOG_DEBUG, "Discovered crash report: %s\n",
				report_filename);

		xmlHashAddEntry(crashes, report_filename,
				xmlStrdup((xmlChar *)""));
		free(report_filename);

		// Remove the marker file
		marker_path = path_in_core_dumps(entry->d_name);
		if (unlink(marker_path) != 0) {
			LOG_MSG (LOG_ERR, "Couldn't unlink marker file %s\n",
					entry->d_name);
		}
		free(marker_path);

		// Rewind directory if its contents were changed
		rewinddir(dir);
	}

	closedir(dir);
}

/**
 * Keeps collecting telemetry URLs from crash-reporter's upload log until either
 * all are fetched or timeout is reached.
 *
 * @param crashes xmlHashTablePtr pre-filled with crash-log filenames
 *
 */
LOCAL void collect_urls_from_uploadlog_timeout (xmlHashTablePtr crashes)
{
	int inotify_fd = -1;
	int inotify_wd = -1;

	while (collect_urls_from_uploadlog(crashes)) {
		const size_t BUFFER_LEN = 1024;
		char buffer[BUFFER_LEN];

		if (opts.core_upload_timeout == 0) {
			LOG_MSG(LOG_DEBUG, "%s: core upload timeout not set, "
					"proceeding immediately", PROGNAME);
			return;
		}

		if (inotify_fd == -1) {
			// First time in loop, initialize inotify
			LOG_MSG(LOG_DEBUG, "%s: waiting for core uploads to finish",
					PROGNAME);

			inotify_fd = inotify_init();
			if (inotify_fd < 0) {
				LOG_MSG(LOG_ERR, "%s: Couldn't initialize inotify",
						PROGNAME);
				return;
			}

			inotify_wd = inotify_add_watch(inotify_fd,
					opts.rich_core_dumps, IN_DELETE);
			if (inotify_wd == -1) {
				LOG_MSG(LOG_ERR, "%s: Couldn't start watching "
						"core dump directory", PROGNAME);
				break;
			}
		}

		if (opts.core_upload_timeout > 0) {
			fd_set set;
			struct timeval timeout;
			int res;

			FD_ZERO(&set);
			FD_SET(inotify_fd, &set);
			timeout.tv_sec = opts.core_upload_timeout;
			timeout.tv_usec = 0;

			res = select(inotify_fd + 1, &set, NULL, NULL, &timeout);
			if (res == -1) {
				LOG_MSG(LOG_ERR, "%s: Error while waiting for core upload",
					PROGNAME);
				break;
			} else if (res == 0) {
				LOG_MSG(LOG_ERR, "%s: Waiting for core upload timed out, proceeding anyway",
					PROGNAME);
				break;
			}
		}

		if (read(inotify_fd, buffer, BUFFER_LEN) < 0) {
			LOG_MSG(LOG_ERR, "%s: Couldn't read from inotify",
				PROGNAME);
		}
	}

	if (inotify_wd != -1) {
		inotify_rm_watch(inotify_fd, inotify_wd);
	}
	close(inotify_fd);
}

/**
 * Collects information about crash reports created during a specified test case
 * run. Function fills given hash table with pairs having crash report filename
 * as a key and its corresponding report URL on a telemetry server as a value.
 *
 * Moreover, if the crash report wasn't for whatever reason uploaded to the
 * server, this function downloads it into testrunner's output directory from
 * the device.
 *
 * @param uuid an identifier of a test case
 * @param crashes a hash table where to store found crash info
 * @return 1, if any reports are found, otherwise 0
 */
LOCAL int fetch_rich_core_dumps (const char *uuid, xmlHashTablePtr crashes)
{
	collect_crash_reports (uuid, crashes);

	if (xmlHashSize(crashes) == 0) {
		LOG_MSG (LOG_DEBUG, "%s: Rich core dumps not found with UUID: %s\n",
			PROGNAME, uuid);
		return 0;
	}

	collect_urls_from_uploadlog_timeout (crashes);

	xmlHashScan(crashes, (xmlHashScanner)fetch_leftover_report, NULL);

	return 1;
}

/**
 * Stores current test case UUID into a file to be read by rich-core-dumper
 * @param uuid Identifier if the test case
 */
LOCAL void set_device_core_pattern (const char *uuid)
{
	char *marker_file_path = path_in_core_dumps(TESTCASE_UUID_FILENAME);
	FILE *file = fopen(marker_file_path, "w");
	size_t uuid_len;

	if (!file) {
		LOG_MSG (LOG_ERR, "%s: Couldn't create %s\n", PROGNAME,
				marker_file_path);
		goto out;
	}

	uuid_len = strlen(uuid);
	if (fwrite(uuid, sizeof (char), uuid_len, file) != uuid_len) {
		LOG_MSG (LOG_ERR, "%s: Couldn't write UUID for test case %s\n",
				PROGNAME, uuid);
	}

out:
	free(marker_file_path);
	if (file) {
		fclose(file);
	}
}

/**
 * Removes test case UUID marker file from rich-core-dumper's output directory
 */
LOCAL void unset_device_core_pattern ()
{
	char *marker_file_path = path_in_core_dumps(TESTCASE_UUID_FILENAME);
	unlink(marker_file_path);
	free(marker_file_path);
}

#ifdef ENABLE_EVENTS
/** Process event
 *  @param data event data
 *  @param user step data
 *  @return 1 if event is successfully processed, 0 if not
 */
LOCAL int event_execute (const void *data, const void *user)
{
	td_event *event = (td_event *)data;
	int ret = 0;

	if (event->type == EVENT_TYPE_SEND) {
		ret = send_event(event);
	}

	if (event->type == EVENT_TYPE_WAIT) {
		ret = wait_for_event(event);
	}
	
	return ret;
}
#endif	/* ENABLE_EVENTS */
/** Process step data. execute one step from case.
 *  @param data step data
 *  @param user case data
 *  @return 1 if step is passed 0 if not
 */
LOCAL int step_execute (const void *data, const void *user) 
{
	int res = CASE_PASS;
	td_step *step = (td_step *)data;
	td_case *c = (td_case *)user;
	td_case dummy;
	exec_data edata;

	cur_step_num++;

	memset (&edata, 0x0, sizeof (exec_data));

	LOG_MSG (LOG_DEBUG, "Value of control %d and bail_out %d",
					 step->control, bail_out);

	/* If step is forced reboot mark start time and wait for reboot */
	if (!bail_out && step->control == CONTROL_REBOOT) {
		step->start = time(NULL);
		wait_for_reboot(step->control);
		step->end = time(NULL);
		/* If no bail out is set, reboot succeeded */
		if(!bail_out) {
			step->has_result = 1;
			/* Execute post_reboot_steps after forced reboot */
			if (xmlListSize (c->post_reboot_steps) > 0) {
				cur_case_name = (xmlChar *)"post_reboot_steps";
				cur_step_num = 0;
				memset (&dummy, 0x0, sizeof (td_case));
				dummy.case_res = CASE_PASS;
				dummy.dummy = 1;
				LOG_MSG (LOG_INFO, "Executing post reboot steps");
				xmlListWalk (c->post_reboot_steps, prepost_steps_execute, &dummy);
				if (dummy.case_res != CASE_PASS) {
					step->return_code = step->expected_result +1;
					res = CASE_FAIL;
					step->failure_info = xmlCharStrdup("post reboot steps failed");
					c->failure_info = xmlCharStrdup ((char *)
									step->failure_info);
					LOG_MSG (LOG_INFO, "FAILURE INFO: %s",
							step->failure_info);
				}
			}
			goto out;
		}

	}

	if (bail_out) {
		/* If forced reboot failed set failure_info */
		if (step->control == CONTROL_REBOOT) {
			bail_out = TESTRUNNER_LITE_REMOTE_FAIL;
			global_failure =
						"earlier connection failure";
			step->failure_info = xmlCharStrdup("connection failure");
			c->failure_info = xmlCharStrdup ((char *)
							 step->failure_info);
			LOG_MSG (LOG_INFO, "FAILURE INFO: %s",
					 step->failure_info);
		}
		step->has_result = 1;
		step->return_code = bail_out;
		/* Dont set global_failure again if step was forced reboot case */
		if (global_failure && step->control != CONTROL_REBOOT) {
			step->failure_info = xmlCharStrdup (global_failure);
			if (!c->failure_info) {
				c->failure_info = xmlCharStrdup(global_failure);
			}
		}

		c->case_res = CASE_FAIL;

		return 1;
	}

#ifdef ENABLE_EVENTS
	if (step->event) {
		/* just process the event */
		if (!event_execute(step->event, step)) {
			step->return_code = 1;
			res = CASE_FAIL;
			LOG_MSG (LOG_INFO, "EVENT: '%s' failed\n",
					 step->event->resource);
		}
		step->has_result = 1;
		goto out;
	}
#endif	/* ENABLE_EVENTS */
	
	if (step->manual) {
		if (c->dummy) {
			LOG_MSG (LOG_WARNING, 
				 "manual pre/post steps not supported");
			goto out;
		}
		if (!c->gen.manual)
			LOG_MSG (LOG_WARNING, "Executing manual step from "
				 "automatic case %s "
				 "(generally not a good idea)",
				 c->gen.name);
		res = execute_manual (step);
		goto out;
	}
	
	init_exec_data(&edata);
	
	edata.control = step->control;
	edata.redirect_output = REDIRECT_OUTPUT;
	edata.soft_timeout = c->gen.timeout;
	edata.hard_timeout = COMMON_HARD_TIMEOUT;
	
	if (step->step) {
		execute((char*)step->step, &edata);
		
		if (step->stdout_) free (step->stdout_);
		if (step->stderr_) free (step->stderr_);
		if (step->failure_info) free (step->failure_info);
		
		if (edata.stdout_data.buffer) {
			step->stdout_ = edata.stdout_data.buffer;
		}
		if (edata.stderr_data.buffer) {
			step->stderr_ = edata.stderr_data.buffer;
		}

		/* If case is expected to reboot the device */
		if (step->control == CONTROL_REBOOT_EXPECTED) {
			/* Connection failure detected, wait for reboot and pass the case
			 * if reboot was succesful */
			if(bail_out == TESTRUNNER_LITE_REMOTE_FAIL) {
				wait_for_reboot(step->control);
				edata.end_time = time(NULL);
				if(!bail_out) {
					edata.result = step->expected_result;
					global_failure = NULL;
					/* Execute post_reboot_steps after expected reboot */
					if (xmlListSize (c->post_reboot_steps) > 0) {
						cur_case_name = (xmlChar *)"post_reboot_steps";
						cur_step_num = 0;
						memset (&dummy, 0x0, sizeof (td_case));
						dummy.case_res = CASE_PASS;
						dummy.dummy = 1;
						LOG_MSG (LOG_INFO, "Executing post reboot steps");
						xmlListWalk (c->post_reboot_steps, prepost_steps_execute, &dummy);
						if (dummy.case_res != CASE_PASS) {
							step->has_result = 1;
							step->return_code = step->expected_result +1;
							res = CASE_FAIL;
							step->failure_info = xmlCharStrdup("post reboot steps failed");
							c->failure_info = xmlCharStrdup ((char *)
											step->failure_info);
							LOG_MSG (LOG_INFO, "FAILURE INFO: %s",
									step->failure_info);
							goto out;
						}
					}

				} else {
					bail_out = TESTRUNNER_LITE_REMOTE_FAIL;
					global_failure =
								"earlier connection failure";
					step->failure_info = xmlCharStrdup("connection failure");
					c->failure_info = xmlCharStrdup ((char *)
									 step->failure_info);
					LOG_MSG (LOG_INFO, "FAILURE INFO: %s",
					 step->failure_info);

					step->has_result = 1;
					step->return_code = bail_out;
					c->case_res = CASE_FAIL;
					goto out;
				}
			} else {
				if (edata.failure_info.buffer) {
					step->failure_info = edata.failure_info.buffer;
					c->failure_info = xmlCharStrdup ((char *)
									 step->failure_info);

					LOG_MSG (LOG_INFO, "FAILURE INFO: %s",
						 step->failure_info);
				}
				step->has_result = 1;
				step->return_code = step->expected_result +1;
				res = CASE_FAIL;
				goto out;
			}
		} else {
			if (edata.failure_info.buffer) {
				step->failure_info = edata.failure_info.buffer;
				c->failure_info = xmlCharStrdup ((char *)
								 step->failure_info);

				LOG_MSG (LOG_INFO, "FAILURE INFO: %s",
					 step->failure_info);
			}
		}

		step->pgid = edata.pgid; 
		step->pid = edata.pid;
		step->has_result = 1;
		step->return_code = edata.result;
		step->start = edata.start_time;
		step->end = edata.end_time;

		/*
		** Post and pre steps fail only if the expected result is 
		*  specified
		*/
		if (c->dummy) 
			if (!step->has_expected_result)
				goto out;
		/*
		** Testrunner-lite may have killed the step command on timeout
		** in this case we do not trust the return value.
		*/
		if (edata.signaled) { 
			step->fail = 1;
			LOG_MSG (LOG_INFO, "STEP: %s terminated by signal %d",
				 step->step, edata.signaled);
			res = CASE_FAIL;
		} else if (step->return_code != step->expected_result) {
			LOG_MSG (LOG_INFO, "STEP: %s return %d expected %d",
				 step->step, step->return_code, 
				 step->expected_result);
			res = CASE_FAIL;
		}
	}
 out:
	if (res != CASE_PASS)
		c->case_res = res;
	
	
	return (res == CASE_PASS);
}
/* ------------------------------------------------------------------------- */
/** Process pre/post steps.
 *  @param data steps data
 *  @param user dummy case data
 *  @return 1 always
 */
LOCAL int prepost_steps_execute (const void *data, const void *user)
{
	td_steps *steps = (td_steps *)data;
	td_case *dummy = (td_case *)user;
	
	dummy->gen.timeout = steps->timeout;
	
	if (xmlListSize(steps->steps) > 0) {
		xmlListWalk (steps->steps, step_execute, dummy);
	}
	
	return 1;
}
/* ------------------------------------------------------------------------- */
/** Set fail result for test step
 *  @param data step data
 *  @param user failure info string
 *  @return 1 always
 */
LOCAL int step_result_fail (const void *data, const void *user) 
{
	td_step *step = (td_step *)data;
	char *failure_info = (char *)user;

	step->has_result = 1;
	step->fail = 1;
	step->failure_info = xmlCharStrdup (failure_info);
	
	return 1;
}

/* ------------------------------------------------------------------------- */
/** Do step post processing. Mainly to ascertain that no dangling processes are
 *  left behind.
 *  @param data step data
 *  @param user case data
 *  @return 1 always
 */
LOCAL int step_post_process (const void *data, const void *user) 
{
	td_step *step = (td_step *)data;
	td_case *c = (td_case *)user;

	/* No post processing for manual steps ... */
	if (step->manual) 
		goto out;
	/* ... or filtered ones ... */
	if (c->filtered)
		goto out;

	/* ... or ones that are not run ... */
	if (!step->start)
		goto out;

	/* ... or ones that do not have process group ... */
	if (!step->pgid)
		goto out;

	/* The required PID file from remote has been cleaned already.
	   Thus commented this useless remote_kill call */
	/* if (opts.remote_executor && !bail_out) { */
	/* 	remote_kill (opts.remote_executor, step->pid, SIGKILL); */
	/* } */

	if (step->pgid > 0) {
		kill_pgroup(step->pgid, SIGKILL);
	}
	
 out:
	return 1;
}
/* ------------------------------------------------------------------------- */
/** Process case data. execute steps in case.
 *  @param data case data
 *  @param user set data
 *  @return 1 always
 */

LOCAL int process_case (const void *data, const void *user) 
{
	td_case *c = (td_case *)data;
	char uuid_buf[36];
	uuid_t uuid_gen;
	char *pos = NULL;

	if (c->gen.manual && !opts.run_manual) {
		LOG_MSG(LOG_DEBUG, "Skipping manual case %s",
			c->gen.name);
		c->filtered = 1;
		return 1;
	}
	if (!c->gen.manual && !opts.run_automatic) {
		LOG_MSG(LOG_DEBUG, "Skipping automatic case %s",
			c->gen.name);
		c->filtered = 1;
		return 1;
	}
	if (filter_case (c)) {
		LOG_MSG (LOG_INFO, "Test case %s is filtered", c->gen.name);
		return 1;
	}
	if (c->state && !xmlStrcmp (c->state, BAD_CAST "Design")) {
		LOG_MSG (LOG_INFO, "Skipping case in Design state (%s)",
			 c->gen.name);
		c->case_res = CASE_NA;
		return 1;
	}

	cur_case_name = c->gen.name;
	LOG_MSG (LOG_INFO, "Starting test case %s", c->gen.name);
	casecount++;

	if (opts.rich_core_dumps != NULL) {
		/* Create UUID to map test case and rich-core dump. */
		uuid_generate (uuid_gen);
		
		if (uuid_is_null (uuid_gen)) {
			LOG_MSG (LOG_WARNING, "Failed to generate UUID.");
		}
		else {
			uuid_unparse (uuid_gen, uuid_buf);
			/* UUID format is xxxx-xxxx-xxxx-xxxx. Replace dashes (-) */
			/* with zeros (0). */
			while ((pos = strchr(uuid_buf, '-')) != NULL) {
				*pos = '0';
			}
			set_device_core_pattern (uuid_buf);
		}
	}

	if (opts.measure_power) {
		if (system("hat_ctrl -stream:5:s1-2:f" MEASUREMENT_FILE
			   ":0 > /dev/null 2>&1") != 0) {
			LOG_MSG (LOG_WARNING, "Failure in power measurement initialization");
		}
	}
	
	c->case_res = CASE_PASS;
	if (c->gen.timeout == 0)
		c->gen.timeout = COMMON_SOFT_TIMEOUT; /* the default one */
	
	if (c->gen.manual && opts.run_manual)
		pre_manual (c);
	if (xmlListSize (c->steps) == 0) {
		LOG_MSG (LOG_WARNING, "Case with no steps (%s).",
			 c->gen.name);
		c->case_res = CASE_NA;
	}
	cur_step_num = 0;
	
	xmlListWalk (c->steps, step_execute, data);
	xmlListWalk (c->steps, step_post_process, data);
	
	if (c->gen.manual && opts.run_manual)
		post_manual (c);

	if (opts.measure_power) {
		if (system("hat_ctrl -stream:0 > /dev/null 2>&1")) {
			LOG_MSG (LOG_WARNING, "Failure in stopping power measurement");
		}
		process_current_measurement(MEASUREMENT_FILE, c);
	}

	if (opts.rich_core_dumps != NULL) {
		unset_device_core_pattern();
		if (fetch_rich_core_dumps (uuid_buf, c->crashes)) {
			c->rich_core_uuid = xmlCharStrdup (uuid_buf);
		}
	}
	xmlListWalk (c->gets, process_get_case, c);
	
	LOG_MSG (LOG_INFO, "Finished test case %s Result: %s",
		 c->gen.name, case_result_str(c->case_res));
	passcount += (c->case_res == CASE_PASS);
	failcount += (c->case_res == CASE_FAIL);
	return 1;
}
/* ------------------------------------------------------------------------- */
/** Set case result to fail
 *  @param data case data
 *  @param user failure info
 *  @return 1 always
 */
LOCAL int case_result_fail (const void *data, const void *user)
{

	td_case *c = (td_case *)data;
	char *failure_info = (char *)user;

	LOG_MSG (LOG_DEBUG, "Setting FAIL result for case %s", c->gen.name);

	c->case_res = CASE_FAIL;
	c->failure_info = xmlCharStrdup (failure_info);

	xmlListWalk (c->steps, step_result_fail, user);
	
	return 1;
}
/* ------------------------------------------------------------------------- */
/** Process set get data. 
 *  @param data get file data
 *  @param user not used
 *  @return 1 always
 */
LOCAL int process_get (const void *data, const void *user)
{

	td_file *file = (td_file *)data;
	char *command;
	char *fname;
	char *tmpname;
	exec_data edata;
	char *p;
	char *executor = opts.remote_executor;
	int command_len;
#ifdef ENABLE_LIBSSH2
	int key_param_len = 0;
	char *remote = opts.target_address;
#endif
	if (bail_out) {
		return 1;
	}

	memset (&edata, 0x0, sizeof (exec_data));
	init_exec_data(&edata);
	edata.soft_timeout = COMMON_SOFT_TIMEOUT;
	edata.hard_timeout = COMMON_HARD_TIMEOUT;

	if (opts.chroot_folder) {
		/* Tell executor not to execute cp command in chroot
		   but in "normal" environment */
		edata.disobey_chroot = 1;

		/* source file must be prefixed with chroot dir */
		fname = (char *)malloc(strlen(opts.chroot_folder) +
				       strlen((char *)file->filename) + 2);
		strcpy(fname, opts.chroot_folder);
		strcat(fname, "/");

		tmpname = malloc (strlen((char *)file->filename) + 1);
		trim_string ((char *)file->filename, tmpname);

		strcat(fname, tmpname);
		free(tmpname);
	} else {
		fname = malloc (strlen((char *)file->filename) + 1);
		trim_string ((char *)file->filename, fname);
	}
	command_len = strlen ("rm -f ") + strlen (fname) + 1;
	/*
	** Compose command 
	*/
#ifdef ENABLE_LIBSSH2
	if (opts.libssh2) {
		if (opts.ssh_key) {
			/* length of "-i keyfile + \'0'" */
			key_param_len = strlen(opts.ssh_key) + 3 + 1;			
		} else {
			key_param_len = 1;
		}

		char key_param[key_param_len];

		if (opts.ssh_key) {
			snprintf(key_param, key_param_len, "-i %s", opts.ssh_key);
		} else {
			key_param[0] = '\0';
		}
		
		opts.target_address = NULL; /* execute locally */
		command_len = strlen ("scp ") +
			strlen (opts.username) + 1 +
			strlen (fname) +
			strlen (opts.output_folder) +
			strlen (remote) + 30 + 
			key_param_len;
		command = (char *)malloc (command_len);
		if (opts.target_port)
			snprintf (command, command_len,
				  "scp -P %u ", opts.target_port);
		else
			snprintf (command, command_len, "scp ");
		p = (char *)(command + (strlen (command)));
		snprintf (p, command_len - strlen(command),
			  "%s %s@%s:\'%s\' %s", key_param, opts.username,
			  remote, fname, opts.output_folder);
	} else
#endif
	if (executor) {
		opts.remote_executor = NULL; /* execute locally */
		command = replace_string (opts.remote_getter, "<FILE>", fname);
		p = command;
		command = replace_string (command, "<DEST>", opts.output_folder);
		free(p);
	} else {
		command_len = strlen ("cp ") + strlen (fname) +
			strlen (opts.output_folder) + 2;
		command = (char *)malloc (command_len);
		snprintf (command, command_len, "cp %s %s", fname,
			 opts.output_folder);
	}

	LOG_MSG (LOG_DEBUG, "%s:  Executing command: %s", PROGNAME, command);
	/*
	** Execute it
	*/
	execute(command, &edata);

	if (edata.result) {
		LOG_MSG (LOG_INFO, "%s: %s failed: %s\n", PROGNAME, command,
			 (char *)(edata.stderr_data.buffer ?
				  edata.stderr_data.buffer : 
				  BAD_CAST "no info available"));
	}
#ifdef ENABLE_LIBSSH2
	opts.target_address = remote;
#endif
	opts.remote_executor = executor;
	if (edata.stdout_data.buffer) free (edata.stdout_data.buffer);
	if (edata.stderr_data.buffer) free (edata.stderr_data.buffer);
	if (edata.failure_info.buffer) free (edata.failure_info.buffer);

	if (!file->delete_after)
		goto out;

	memset (&edata, 0x0, sizeof (exec_data));
	init_exec_data(&edata);
	edata.soft_timeout = COMMON_SOFT_TIMEOUT;
	edata.hard_timeout = COMMON_HARD_TIMEOUT;
	snprintf (command, command_len, "rm -f %s", fname);
	LOG_MSG (LOG_DEBUG, "%s:  Executing command: %s", PROGNAME, command);
	execute(command, &edata);
	if (edata.result) {
		LOG_MSG (LOG_WARNING, "%s: %s failed: %s\n", PROGNAME, command,
			 (char *)(edata.stderr_data.buffer ?
				  edata.stderr_data.buffer : 
				  BAD_CAST "no info available"));
	}
	if (edata.stdout_data.buffer) free (edata.stdout_data.buffer);
	if (edata.stderr_data.buffer) free (edata.stderr_data.buffer);
	if (edata.failure_info.buffer) free (edata.failure_info.buffer);

 out:
	free (command);
	free (fname);
	return 1;
}
/* ------------------------------------------------------------------------- */
/** Process case get data. 
 *  @param data get file data
 *  @param user case data
 *  @return 1 always
 */
LOCAL int process_get_case (const void *data, const void *user)
{
	int ret = 0;
	td_file *file = (td_file *)data;
	td_case *c = (td_case *)user;
	char *trimmed_name, *fname, *filename, *failure_str = NULL;
	int measurement_verdict = CASE_PASS;
	size_t len;

	ret = process_get (data, NULL);
	if (!ret)
		LOG_MSG (LOG_WARNING, "get file processing failed");

	if (bail_out)
		return 1;

	/* return if file not specified to contain measurement data */
	if (!file->measurement)
		return 1;
	
	trimmed_name = malloc (strlen ((char *)file->filename) + 1);
	trim_string ((char *)file->filename, trimmed_name);
	fname = strrchr (trimmed_name, '/');
	if (!fname)
		fname = trimmed_name;
	else
		fname ++;
	len = strlen((char *)fname) + 2 + strlen (opts.output_folder);
	filename = malloc (len);
	snprintf (filename, len, "%s%s", opts.output_folder, fname);

	ret = get_measurements (filename, c, file->series);
	free (trimmed_name);
	free (filename);
	
	/* Evaluate measurements only if case is otherwise passed ... */
	if (c->case_res != CASE_PASS)
		return 1;
	/* ... and -M flag is not set */ 
	if (opts.no_measurement_verdicts)
		return 1;
	
	/* ... and measurement getting was succesfull */
	if (ret) {
		c->case_res = CASE_FAIL;
		c->failure_info = xmlCharStrdup ("Failed to process "
						 "measurement file");
		return 1;
	}
	
	ret = eval_measurements (c, &measurement_verdict,
				 &failure_str, file->series);
	if (ret)
		return 1;
	if (measurement_verdict == CASE_FAIL) {
		LOG_MSG (LOG_INFO, "Failing test case %s (%s)", 
			 c->gen.name, failure_str ? failure_str : 
			 "no info");
		c->case_res = CASE_FAIL;
		if (failure_str) 
			c->failure_info = BAD_CAST failure_str;
	}
			
	return 1;
}

/* ------------------------------------------------------------------------- */
/** Process test definition
 *  @param *td Test definition data
 */
LOCAL void process_td (td_td *td)
{
	current_td = td;
	write_td_start (td);
}
/* ------------------------------------------------------------------------- */
/** Do test definition cleaning
 */
LOCAL void end_td ()
{
	write_td_end (current_td);
	td_td_delete (current_td);
	current_td = NULL;
}
/* ------------------------------------------------------------------------- */
/** Process hwiddetect: Run detector command and store result in current td
 */
LOCAL void process_hwiddetect ()
{
	exec_data edata;
	char* trimmed = NULL;
	size_t length = 0;

	if (current_td && current_td->hw_detector) {
		init_exec_data(&edata);
		edata.redirect_output = REDIRECT_OUTPUT;
		edata.soft_timeout = COMMON_SOFT_TIMEOUT;
		edata.hard_timeout = COMMON_HARD_TIMEOUT;

		execute((char*)current_td->hw_detector, &edata);

		if (edata.result != EXIT_SUCCESS) {
			LOG_MSG (LOG_WARNING, "Running HW ID detector "
				 "failed with return value %d",
				 edata.result);
		} else if (edata.stdout_data.buffer) {
			/* remove possible whitespace, linefeeds, etc. */
			length = strlen((char*)edata.stdout_data.buffer);
			trimmed = (char*)malloc(length + 1);
			trim_string((char*)edata.stdout_data.buffer, trimmed);

			current_td->detected_hw = xmlCharStrdup(trimmed);
			LOG_MSG (LOG_INFO, "Detected HW ID '%s'",
				 current_td->detected_hw);
		}

		clean_exec_data(&edata);
	}

	free(trimmed);
}
/* ------------------------------------------------------------------------- */
/** Do processing on suite, currently just writes the pre suite tag to results
 *  @param s suite data
 */
LOCAL void process_suite (td_suite *s)
{
	LOG_MSG (LOG_INFO, "Test suite: %s", s->gen.name);

	write_pre_suite (s);
	current_suite = s;
	
}
/* ------------------------------------------------------------------------- */
/** Suite end function, write suite and delete the current_suite
 */
LOCAL void end_suite ()
{
	write_post_suite (current_suite);
	td_suite_delete (current_suite);
	current_suite = NULL;
}
/* ------------------------------------------------------------------------- */
/** Process set data. Walk through cases and free set when done.
 *  @param s set data
 */
LOCAL void process_set (td_set *s)
{
	td_case dummy;
	td_steps *steps;
	/*
	** Check that the set is not filtered
	*/
	if (filter_set (s)) {
		LOG_MSG (LOG_INFO, "Test set %s is filtered", s->gen.name);
		goto skip_all;
	}

	/*
	** User defined HW ID based filtering
	*/
	if (s->gen.hwid && current_td->detected_hw &&
	    list_contains((const char *)s->gen.hwid, 
			 (const char *) current_td->detected_hw, ",") == 0) {
		LOG_MSG (LOG_INFO, "Test set %s is filtered based on HW ID",
			 s->gen.name);
		goto skip_all;
	}

	/*
	** Check that the set is supposed to be executed in the current env
	*/
	s->environment = xmlCharStrdup (opts.environment);
	if (!xmlListSearch (s->environments, opts.environment)) {
		LOG_MSG (LOG_INFO, "Test set %s not run on "
			 "environment: %s", 
			 s->gen.name, opts.environment);
		goto skip_all;
	}
	current_set = s;
	LOG_MSG (LOG_INFO, "Test set: %s", s->gen.name);
	write_pre_set (s);

	if (xmlListSize (s->pre_steps) > 0) {
		cur_case_name = (xmlChar *)"pre_steps";
		cur_step_num = 0;
		memset (&dummy, 0x0, sizeof (td_case));
		dummy.case_res = CASE_PASS;
		dummy.dummy = 1;
		LOG_MSG (LOG_INFO, "Executing pre steps");
		xmlListWalk (s->pre_steps, prepost_steps_execute, &dummy);
		if (dummy.case_res != CASE_PASS) {
			LOG_MSG (LOG_INFO, "Pre steps failed. "
				 "Test set %s aborted.", s->gen.name); 
			xmlListWalk (s->cases, case_result_fail, 
				     global_failure ? global_failure :
				     "pre_steps failed");
			goto short_circuit;
		}
	}
	
	xmlListWalk (s->cases, process_case, s);

	if (opts.resume_testrun != RESUME_TESTRUN_ACTION_NONE) {
		wait_for_resume_execution();
	}

	if (xmlListSize (s->post_steps) > 0) {
		LOG_MSG (LOG_INFO, "Executing post steps");
		cur_case_name = (xmlChar *)"post_steps";
		cur_step_num = 0;
		memset (&dummy, 0x0, sizeof (td_case));
		dummy.case_res = CASE_PASS;
		dummy.dummy = 1;
		xmlListWalk (s->post_steps, prepost_steps_execute, &dummy);
		if (dummy.case_res == CASE_FAIL)
			LOG_MSG (LOG_INFO, 
				 "Post steps failed for %s.", s->gen.name);
	}
	xmlListWalk (s->gets, process_get, NULL);

	if (opts.resume_testrun == RESUME_TESTRUN_ACTION_EXIT) {
		restore_bail_out_after_resume_execution();
	}

 short_circuit:
	write_post_set (s);
	if (xmlListSize (s->pre_steps) > 0) {
		steps = xmlLinkGetData(xmlListFront(s->pre_steps));
		xmlListWalk (steps->steps, step_post_process, &dummy);
	}
	if (xmlListSize (s->post_steps) > 0) {
		steps = xmlLinkGetData(xmlListFront(s->post_steps));
		xmlListWalk (steps->steps, step_post_process, &dummy);
	}
	xml_end_element();
 skip_all:
	td_set_delete (s);
	return;
}
/* ------------------------------------------------------------------------- */
/* ======================== FUNCTIONS ====================================== */
/* ------------------------------------------------------------------------- */
/** Walks through the whole test definition and executes all suites, 
 *  sets, cases and steps.
 */
void td_process () {
	int retval;
	td_parser_callbacks cbs;

        memset (&cbs, 0x0, sizeof(td_parser_callbacks));
	/*
	** Set callbacks for parser
	*/
	cbs.test_td = process_td;
	cbs.test_td_end = end_td;
	cbs.test_hwiddetect = process_hwiddetect;
	cbs.test_suite = process_suite;
	cbs.test_suite_end = end_suite;
	cbs.test_set = process_set;

	retval = td_register_callbacks (&cbs);
	
	/*
	** Call td_next_node untill error occurs or the end of data is reached
	*/
	LOG_MSG (LOG_INFO, "Starting to run tests...");

	while (td_next_node() == 0);

	LOG_MSG (LOG_INFO, "Finished running tests.");
	LOG_MSG (LOG_INFO, "Executed %d cases. Passed %d Failed %d",
		 casecount, passcount, failcount);
	return; 
}	
/* ------------------------------------------------------------------------- */
/** Name of the currently executed set
 * @return name of the set or NULL
 */
const char *current_set_name ()
{
	if (current_set)
		return (char *)current_set->gen.name;
	return "";
}
/* ------------------------------------------------------------------------- */
/** Name of the currently executed case (can be also "pre/post_steps"
 * @return case name
 */
const char *current_case_name ()
{
	return (char *)cur_case_name;
}
/* ------------------------------------------------------------------------- */
/** Number of the step currently executed
 *  return 0 if no step is executed, > 0 otherwise
 */
int current_step_num ()
{
	return cur_step_num;
}

/* ================= OTHER EXPORTED FUNCTIONS ============================== */
/* None */

/* ------------------------------------------------------------------------- */
/* End of file */
