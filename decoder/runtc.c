/*
 * This file is part of the sigrok-test project.
 *
 * Copyright (C) 2013 Bert Vermeulen <bert@biot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <Python.h>
#include <libsigrokdecode/libsigrokdecode.h>
#include <libsigrok/libsigrok.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <dirent.h>
#include <glib.h>
#ifdef __LINUX__
#include <sched.h>
#endif

int debug = FALSE;
int statistics = FALSE;
char *coverage_report;

struct channel {
	char *name;
	int channel;
};

struct option {
	char *key;
	GVariant *value;
};

struct pd {
	char *name;
	GSList *channels;
	GSList *options;
};

struct output {
	char *pd;
	int type;
	char *class;
	int class_idx;
	char *outfile;
	int outfd;
};

struct cvg {
	int num_lines;
	int num_missed;
	float coverage;
	GSList *missed_lines;
};

struct cvg *get_mod_cov(PyObject *py_cov, char *module_name);
void cvg_add(struct cvg *dst, struct cvg *src);
struct cvg *cvg_new(void);
gboolean find_missed_line(struct cvg *cvg, char *linespec);

static void logmsg(char *prefix, FILE *out, const char *format, va_list args)
{
	if (prefix)
		fprintf(out, "%s", prefix);
	vfprintf(out, format, args);
	fprintf(out, "\n");
}

static void DBG(const char *format, ...)
{
	va_list args;

	if (!debug)
		return;
	va_start(args, format);
	logmsg("DBG: runtc: ", stdout, format, args);
	va_end(args);
}

static void ERR(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	logmsg("Error: ", stderr, format, args);
	va_end(args);
}

static int sr_log(void *cb_data, int loglevel, const char *format, va_list args)
{
	(void)cb_data;

	if (loglevel == SR_LOG_ERR || loglevel == SR_LOG_WARN)
		logmsg("Error: sr: ", stderr, format, args);
	else if (debug)
		logmsg("DBG: sr: ", stdout, format, args);

	return SRD_OK;
}

static int srd_log(void *cb_data, int loglevel, const char *format, va_list args)
{
	(void)cb_data;

	if (loglevel == SRD_LOG_ERR || loglevel == SRD_LOG_WARN)
		logmsg("Error: srd: ", stderr, format, args);
	else if (debug)
		logmsg("DBG: srd: ", stdout, format, args);

	return SRD_OK;
}

static void usage(char *msg)
{
	if (msg)
		fprintf(stderr, "%s\n", msg);

	printf("Usage: runtc [-dPpoiOf]\n");
	printf("  -d  Debug\n");
	printf("  -P  <protocol decoder>\n");
	printf("  -p  <channelname=channelnum> (optional)\n");
	printf("  -o  <channeloption=value> (optional)\n");
	printf("  -i <input file>\n");
	printf("  -O <output-pd:output-type[:output-class]>\n");
	printf("  -f <output file> (optional)\n");
	printf("  -c <coverage report> (optional)\n");
	exit(msg ? 1 : 0);

}

/* This is a neutered version of libsigrokdecode's py_str_as_str(). It
 * does no error checking, but then the only strings it processes are
 * generated by Python's repr(), so are known good. */
static char *py_str_as_str(const PyObject *py_str)
{
	PyObject *py_encstr;
	char *str, *outstr;

	py_encstr = PyUnicode_AsEncodedString((PyObject *)py_str, "utf-8", NULL);
	str = PyBytes_AS_STRING(py_encstr);
	outstr = g_strdup(str);
	Py_DecRef(py_encstr);

	return outstr;
}

static void srd_cb_py(struct srd_proto_data *pdata, void *cb_data)
{
	struct output *op;
	PyObject *pydata, *pyrepr;
	GString *out;
	char *s;

	DBG("Python output from %s", pdata->pdo->di->inst_id);
	op = cb_data;
	pydata = pdata->data;
	DBG("ptr %p", pydata);

	if (strcmp(pdata->pdo->di->inst_id, op->pd))
		/* This is not the PD selected for output. */
		return;

	if (!(pyrepr = PyObject_Repr(pydata))) {
		ERR("Invalid Python object.");
		return;
	}
	s = py_str_as_str(pyrepr);
	Py_DecRef(pyrepr);

	/* Output format for testing is '<ss>-<es> <inst-id>: <repr>\n'. */
	out = g_string_sized_new(128);
	g_string_printf(out, "%" PRIu64 "-%" PRIu64 " %s: %s\n",
			pdata->start_sample, pdata->end_sample,
			pdata->pdo->di->inst_id, s);
	g_free(s);
	if (write(op->outfd, out->str, out->len) == -1)
		ERR("SRD_OUTPUT_PYTHON callback write failure!");
	DBG("wrote '%s'", out->str);
	g_string_free(out, TRUE);

}

static void srd_cb_bin(struct srd_proto_data *pdata, void *cb_data)
{
	struct srd_proto_data_binary *pdb;
	struct output *op;
	GString *out;
	unsigned int i;

	DBG("Binary output from %s", pdata->pdo->di->inst_id);
	op = cb_data;
	pdb = pdata->data;

	if (strcmp(pdata->pdo->di->inst_id, op->pd))
		/* This is not the PD selected for output. */
		return;

	if (op->class_idx != -1 && op->class_idx != pdb->bin_class)
		/*
		 * This output takes a specific binary class,
		 * but not the one that just came in.
		 */
		return;

	out = g_string_sized_new(128);
	g_string_printf(out, "%" PRIu64 "-%" PRIu64 " %s:",
			pdata->start_sample, pdata->end_sample,
			pdata->pdo->di->inst_id);
	for (i = 0; i < pdb->size; i++) {
		g_string_append_printf(out, " %.2x", pdb->data[i]);
	}
	g_string_append(out, "\n");
	if (write(op->outfd, out->str, out->len) == -1)
		ERR("SRD_OUTPUT_BINARY callback write failure!");

}

static void srd_cb_ann(struct srd_proto_data *pdata, void *cb_data)
{
	struct srd_decoder *dec;
	struct srd_proto_data_annotation *pda;
	struct output *op;
	GString *line;
	int i;
	char **dec_ann;

	DBG("Annotation output from %s", pdata->pdo->di->inst_id);
	op = cb_data;
	pda = pdata->data;
	dec = pdata->pdo->di->decoder;
	if (strcmp(pdata->pdo->di->inst_id, op->pd))
		/* This is not the PD selected for output. */
		return;

	if (op->class_idx != -1 && op->class_idx != pda->ann_class)
		/*
		 * This output takes a specific annotation class,
		 * but not the one that just came in.
		 */
		return;

	dec_ann = g_slist_nth_data(dec->annotations, pda->ann_class);
	line = g_string_sized_new(256);
	g_string_printf(line, "%" PRIu64 "-%" PRIu64 " %s: %s:",
			pdata->start_sample, pdata->end_sample,
			pdata->pdo->di->inst_id, dec_ann[0]);
	for (i = 0; pda->ann_text[i]; i++)
		g_string_append_printf(line, " \"%s\"", pda->ann_text[i]);
	g_string_append(line, "\n");
	if (write(op->outfd, line->str, line->len) == -1)
		ERR("SRD_OUTPUT_ANN callback write failure!");
	g_string_free(line, TRUE);

}

static void sr_cb(const struct sr_dev_inst *sdi,
		const struct sr_datafeed_packet *packet, void *cb_data)
{
	const struct sr_datafeed_logic *logic;
	struct srd_session *sess;
	GVariant *gvar;
	uint64_t samplerate;
	int num_samples;
	static int samplecnt = 0;

	sess = cb_data;

	switch (packet->type) {
	case SR_DF_HEADER:
		DBG("Received SR_DF_HEADER");
		if (sr_config_get(sdi->driver, sdi, NULL, SR_CONF_SAMPLERATE,
				&gvar) != SR_OK) {
			ERR("Getting samplerate failed");
			break;
		}
		samplerate = g_variant_get_uint64(gvar);
		g_variant_unref(gvar);
		if (srd_session_metadata_set(sess, SRD_CONF_SAMPLERATE,
				g_variant_new_uint64(samplerate)) != SRD_OK) {
			ERR("Setting samplerate failed");
			break;
		}
		if (srd_session_start(sess) != SRD_OK) {
			ERR("Session start failed");
			break;
		}
		break;
	case SR_DF_LOGIC:
		logic = packet->payload;
		num_samples = logic->length / logic->unitsize;
		DBG("Received SR_DF_LOGIC: %d samples", num_samples);
		srd_session_send(sess, samplecnt, samplecnt + num_samples,
				logic->data, logic->length);
		samplecnt += logic->length / logic->unitsize;
		break;
	case SR_DF_END:
		DBG("Received SR_DF_END");
		break;
	}

}

static int run_testcase(char *infile, GSList *pdlist, struct output *op)
{
	struct srd_session *sess;
	struct srd_decoder *dec;
	struct srd_decoder_inst *di, *prev_di;
	srd_pd_output_callback cb;
	struct pd *pd;
	struct channel *channel;
	struct option *option;
	GVariant *gvar;
	GHashTable *channels, *opts;
	GSList *pdl, *l;
	int idx;
	int max_channel;
	char **decoder_class;
	struct sr_session *sr_sess;

	if (op->outfile) {
		if ((op->outfd = open(op->outfile, O_CREAT|O_WRONLY, 0600)) == -1) {
			ERR("Unable to open %s for writing: %s", op->outfile,
					strerror(errno));
			return FALSE;
		}
	}

	if (sr_session_load(infile, &sr_sess) != SR_OK)
		return FALSE;

	if (srd_session_new(&sess) != SRD_OK)
		return FALSE;
	sr_session_datafeed_callback_add(sr_sess, sr_cb, sess);
	switch (op->type) {
	case SRD_OUTPUT_ANN:
		cb = srd_cb_ann;
		break;
	case SRD_OUTPUT_BINARY:
		cb = srd_cb_bin;
		break;
	case SRD_OUTPUT_PYTHON:
		cb = srd_cb_py;
		break;
	default:
		return FALSE;
	}
	srd_pd_output_callback_add(sess, op->type, cb, op);

	prev_di = NULL;
	pd = NULL;
	for (pdl = pdlist; pdl; pdl = pdl->next) {
		pd = pdl->data;
		if (srd_decoder_load(pd->name) != SRD_OK)
			return FALSE;

		/* Instantiate decoder and pass in options. */
		opts = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
				(GDestroyNotify)g_variant_unref);
		for (l = pd->options; l; l = l->next) {
			option = l->data;
			g_hash_table_insert(opts, option->key, option->value);
		}
		if (!(di = srd_inst_new(sess, pd->name, opts)))
			return FALSE;
		g_hash_table_destroy(opts);

		/* Map channels. */
		if (pd->channels) {
			channels = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
					(GDestroyNotify)g_variant_unref);
			max_channel = 0;
			for (l = pd->channels; l; l = l->next) {
				channel = l->data;
				if (channel->channel > max_channel)
					max_channel = channel->channel;
				gvar = g_variant_new_int32(channel->channel);
				g_variant_ref_sink(gvar);
				g_hash_table_insert(channels, channel->name, gvar);
			}
			if (srd_inst_channel_set_all(di, channels,
					(max_channel + 8) / 8) != SRD_OK)
				return FALSE;
			g_hash_table_destroy(channels);
		}

		/* If this is not the first decoder in the list, stack it
		 * on top of the previous one. */
		if (prev_di) {
			if (srd_inst_stack(sess, prev_di, di) != SRD_OK) {
				ERR("Failed to stack decoder instances.");
				return FALSE;
			}
		}
		prev_di = di;
	}

	/* Resolve top decoder's class index, so we can match. */
	dec = srd_decoder_get_by_id(pd->name);
	if (op->class) {
		if (op->type == SRD_OUTPUT_ANN)
			l = dec->annotations;
		else if (op->type == SRD_OUTPUT_BINARY)
			l = dec->binary;
		else
			/* Only annotations and binary can have a class. */
			return FALSE;
		idx = 0;
		while (l) {
			decoder_class = l->data;
			if (!strcmp(decoder_class[0], op->class)) {
				op->class_idx = idx;
				break;
			} else
				idx++;
			l = l->next;
		}
		if (op->class_idx == -1) {
			ERR("Output class '%s' not found in decoder %s.",
					op->class, pd->name);
			return FALSE;
		} else
			DBG("Class %s index is %d", op->class, op->class_idx);
	}

	sr_session_start(sr_sess);
	sr_session_run(sr_sess);
	sr_session_stop(sr_sess);

	srd_session_destroy(sess);

	if (op->outfile)
		close(op->outfd);

	return TRUE;
}

static PyObject *start_coverage(GSList *pdlist)
{
	PyObject *py_mod, *py_pdlist, *py_pd, *py_func, *py_args, *py_kwargs, *py_cov;
	GSList *l;
	struct pd *pd;

	DBG("Starting coverage.");

	if (!(py_mod = PyImport_ImportModule("coverage")))
		return NULL;

	if (!(py_pdlist = PyList_New(0)))
		return NULL;
	for (l = pdlist; l; l = l->next) {
		pd = l->data;
		py_pd = PyUnicode_FromFormat("*/%s/*.py", pd->name);
		if (PyList_Append(py_pdlist, py_pd) < 0)
			return NULL;
		Py_DecRef(py_pd);
	}
	if (!(py_func = PyObject_GetAttrString(py_mod, "coverage")))
		return NULL;
	if (!(py_args = PyTuple_New(0)))
		return NULL;
	if (!(py_kwargs = Py_BuildValue("{sO}", "include", py_pdlist)))
		return NULL;
	if (!(py_cov = PyObject_Call(py_func, py_args, py_kwargs)))
		return NULL;
	if (!(PyObject_CallMethod(py_cov, "start", NULL)))
		return NULL;
	Py_DecRef(py_pdlist);
	Py_DecRef(py_args);
	Py_DecRef(py_kwargs);
	Py_DecRef(py_func);

	return py_cov;
}

struct cvg *get_mod_cov(PyObject *py_cov, char *module_name)
{
	PyObject *py_mod, *py_pathlist, *py_path, *py_func, *py_pd;
	PyObject *py_result, *py_missed, *py_item;
	DIR *d;
	struct dirent *de;
	struct cvg *cvg_mod;
	int num_lines, num_missed, linenum, i, j;
	char *path, *linespec;

	if (!(py_mod = PyImport_ImportModule(module_name)))
		return NULL;

	cvg_mod = NULL;
	py_pathlist = PyObject_GetAttrString(py_mod, "__path__");
	for (i = 0; i < PyList_Size(py_pathlist); i++) {
		py_path = PyList_GetItem(py_pathlist, i);
        PyUnicode_FSConverter(PyList_GetItem(py_pathlist, i), &py_path);
		path = PyBytes_AS_STRING(py_path);
		if (!(d = opendir(path))) {
			ERR("Invalid module path '%s'", path);
			return NULL;
		}
		while ((de = readdir(d))) {
			if (strncmp(de->d_name + strlen(de->d_name) - 3, ".py", 3))
				continue;

			if (!(py_func = PyObject_GetAttrString(py_cov, "analysis2")))
				return NULL;
			if (!(py_pd = PyUnicode_FromFormat("%s/%s", path, de->d_name)))
				return NULL;
			if (!(py_result = PyObject_CallFunction(py_func, "O", py_pd)))
				return NULL;
			Py_DecRef(py_pd);
			Py_DecRef(py_func);

			if (!cvg_mod)
				cvg_mod = cvg_new();
			if (PyTuple_Size(py_result) != 5) {
				ERR("Invalid result from coverage of '%s/%s'", path, de->d_name);
				return NULL;
			}
			num_lines = PyList_Size(PyTuple_GetItem(py_result, 1));
			py_missed = PyTuple_GetItem(py_result, 3);
			num_missed = PyList_Size(py_missed);
			cvg_mod->num_lines += num_lines;
			cvg_mod->num_missed += num_missed;
			for (j = 0; j < num_missed; j++) {
				py_item = PyList_GetItem(py_missed, j);
				linenum = PyLong_AsLong(py_item);
				linespec = g_strdup_printf("%s/%s:%d", module_name,
						de->d_name, linenum);
				cvg_mod->missed_lines = g_slist_append(cvg_mod->missed_lines, linespec);
			}
			DBG("Coverage for %s/%s: %d lines, %d missed.",
					module_name, de->d_name, num_lines, num_missed);
			Py_DecRef(py_result);
		}
	}
	if (cvg_mod->num_lines)
		cvg_mod->coverage = 100 - ((float)cvg_mod->num_missed / (float)cvg_mod->num_lines * 100);

	Py_DecRef(py_mod);
	Py_DecRef(py_path);

	return cvg_mod;
}

struct cvg *cvg_new(void)
{
	struct cvg *cvg;

	cvg = calloc(1, sizeof(struct cvg));

	return cvg;
}

gboolean find_missed_line(struct cvg *cvg, char *linespec)
{
	GSList *l;

	for (l = cvg->missed_lines; l; l = l->next)
		if (!strcmp(l->data, linespec))
			return TRUE;

	return FALSE;
}

void cvg_add(struct cvg *dst, struct cvg *src)
{
	GSList *l;
	char *linespec;

	dst->num_lines += src->num_lines;
	dst->num_missed += src->num_missed;
	for (l = src->missed_lines; l; l = l->next) {
		linespec = l->data;
		if (!find_missed_line(dst, linespec))
			dst->missed_lines = g_slist_append(dst->missed_lines, linespec);
	}

}

static int report_coverage(PyObject *py_cov, GSList *pdlist)
{
	PyObject *py_func, *py_mod, *py_args, *py_kwargs, *py_outfile, *py_pct;
	GSList *l, *ml;
	struct pd *pd;
	struct cvg *cvg_mod, *cvg_all;
	float total_coverage;
	int lines, missed, cnt;

	DBG("Making coverage report.");

	/* Get coverage for each module in the stack. */
	lines = missed = 0;
	cvg_all = cvg_new();
	for (cnt = 0, l = pdlist; l; l = l->next, cnt++) {
		pd = l->data;
		if (!(cvg_mod = get_mod_cov(py_cov, pd->name)))
			return FALSE;
		printf("coverage: scope=%s coverage=%.0f%% lines=%d missed=%d "
				"missed_lines=", pd->name, cvg_mod->coverage,
				cvg_mod->num_lines, cvg_mod->num_missed);
		for (ml = cvg_mod->missed_lines; ml; ml = ml->next) {
			if (ml != cvg_mod->missed_lines)
				printf(",");
			printf("%s", (char *)ml->data);
		}
		printf("\n");
		lines += cvg_mod->num_lines;
		missed += cvg_mod->num_missed;
		cvg_add(cvg_all, cvg_mod);
		DBG("Coverage for module %s: %d lines, %d missed", pd->name,
				cvg_mod->num_lines, cvg_mod->num_missed);
	}
	lines /= cnt;
	missed /= cnt;
	total_coverage = 100 - ((float)missed / (float)lines * 100);

	/* Machine-readable stats on stdout. */
	printf("coverage: scope=all coverage=%.0f%% lines=%d missed=%d\n",
			total_coverage, cvg_all->num_lines, cvg_all->num_missed);

	/* Write text report to file. */
	/* io.open(coverage_report, "w") */
	if (!(py_mod = PyImport_ImportModule("io")))
		return FALSE;
	if (!(py_func = PyObject_GetAttrString(py_mod, "open")))
		return FALSE;
	if (!(py_args = PyTuple_New(0)))
		return FALSE;
	if (!(py_kwargs = Py_BuildValue("{ssss}", "file", coverage_report,
			"mode", "w")))
		return FALSE;
	if (!(py_outfile = PyObject_Call(py_func, py_args, py_kwargs)))
		return FALSE;
	Py_DecRef(py_kwargs);
	Py_DecRef(py_func);

	/* py_cov.report(file=py_outfile) */
	if (!(py_func = PyObject_GetAttrString(py_cov, "report")))
		return FALSE;
	if (!(py_kwargs = Py_BuildValue("{sO}", "file", py_outfile)))
		return FALSE;
	if (!(py_pct = PyObject_Call(py_func, py_args, py_kwargs)))
		return FALSE;
	Py_DecRef(py_pct);
	Py_DecRef(py_kwargs);
	Py_DecRef(py_func);

	/* py_outfile.close() */
	if (!(py_func = PyObject_GetAttrString(py_outfile, "close")))
		return FALSE;
	if (!PyObject_Call(py_func, py_args, NULL))
		return FALSE;
	Py_DecRef(py_outfile);
	Py_DecRef(py_func);
	Py_DecRef(py_args);
	Py_DecRef(py_mod);

	return TRUE;
}

int main(int argc, char **argv)
{
	struct sr_context *ctx;
	PyObject *coverage;
	GSList *pdlist;
	struct pd *pd;
	struct channel *channel;
	struct option *option;
	struct output *op;
	int ret, c;
	char *opt_infile, **kv, **opstr;

	op = malloc(sizeof(struct output));
	op->pd = NULL;
	op->type = -1;
	op->class = NULL;
	op->class_idx = -1;
	op->outfd = 1;

	pdlist = NULL;
	opt_infile = NULL;
	pd = NULL;
	coverage = NULL;
	while ((c = getopt(argc, argv, "dP:p:o:i:O:f:c:S")) != -1) {
		switch (c) {
		case 'd':
			debug = TRUE;
			break;
		case 'P':
			pd = g_malloc(sizeof(struct pd));
			pd->name = g_strdup(optarg);
			pd->channels = pd->options = NULL;
			pdlist = g_slist_append(pdlist, pd);
			break;
		case 'p':
		case 'o':
			if (g_slist_length(pdlist) == 0) {
				/* No previous -P. */
				ERR("Syntax error at '%s'", optarg);
				usage(NULL);
			}
			kv = g_strsplit(optarg, "=", 0);
			if (!kv[0] || (!kv[1] || kv[2])) {
				/* Need x=y. */
				ERR("Syntax error at '%s'", optarg);
				g_strfreev(kv);
				usage(NULL);
			}
			if (c == 'p') {
				channel = malloc(sizeof(struct channel));
				channel->name = g_strdup(kv[0]);
				channel->channel = strtoul(kv[1], 0, 10);
				/* Apply to last PD. */
				pd->channels = g_slist_append(pd->channels, channel);
			} else {
				option = malloc(sizeof(struct option));
				option->key = g_strdup(kv[0]);
				option->value = g_variant_new_string(kv[1]);
                g_variant_ref_sink(option->value);
				/* Apply to last PD. */
				pd->options = g_slist_append(pd->options, option);
			}
			break;
		case 'i':
			opt_infile = optarg;
			break;
		case 'O':
			opstr = g_strsplit(optarg, ":", 0);
			if (!opstr[0] || !opstr[1]) {
				/* Need at least abc:def. */
				ERR("Syntax error at '%s'", optarg);
				g_strfreev(opstr);
				usage(NULL);
			}
			op->pd = g_strdup(opstr[0]);
			if (!strcmp(opstr[1], "annotation"))
				op->type = SRD_OUTPUT_ANN;
			else if (!strcmp(opstr[1], "binary"))
				op->type = SRD_OUTPUT_BINARY;
			else if (!strcmp(opstr[1], "python"))
				op->type = SRD_OUTPUT_PYTHON;
			else if (!strcmp(opstr[1], "exception"))
				/* Doesn't matter, we just need it to bomb out. */
				op->type = SRD_OUTPUT_PYTHON;
			else {
				ERR("Unknown output type '%s'", opstr[1]);
				g_strfreev(opstr);
				usage(NULL);
			}
			if (opstr[2])
				op->class = g_strdup(opstr[2]);
			g_strfreev(opstr);
			break;
		case 'f':
			op->outfile = g_strdup(optarg);
			op->outfd = -1;
			break;
		case 'c':
			coverage_report = optarg;
			break;
		case 'S':
			statistics = TRUE;
			break;
		default:
			usage(NULL);
		}
	}
	if (argc > optind)
		usage(NULL);
	if (g_slist_length(pdlist) == 0)
		usage(NULL);
	if (!opt_infile)
		usage(NULL);
	if (!op->pd || op->type == -1)
		usage(NULL);

	sr_log_callback_set(sr_log, NULL);
	if (sr_init(&ctx) != SR_OK)
		return 1;

	srd_log_callback_set(srd_log, NULL);
	if (srd_init(DECODERS_DIR) != SRD_OK)
		return 1;

	if (coverage_report) {
		if (!(coverage = start_coverage(pdlist))) {
			DBG("Failed to start coverage.");
			if (PyErr_Occurred()) {
				PyErr_PrintEx(0);
				PyErr_Clear();
			}
		}
	}

	ret = 0;
	if (!run_testcase(opt_infile, pdlist, op))
		ret = 1;

	if (coverage) {
		DBG("Stopping coverage.");

		if (!(PyObject_CallMethod(coverage, "stop", NULL)))
			ERR("Failed to stop coverage.");
		else if (!(report_coverage(coverage, pdlist)))
			ERR("Failed to make coverage report.");
		else
			DBG("Coverage report in %s", coverage_report);

		if (PyErr_Occurred()) {
			PyErr_PrintEx(0);
			PyErr_Clear();
		}
		Py_DecRef(coverage);
	}

	srd_exit();
	sr_exit(ctx);

	return ret;
}
