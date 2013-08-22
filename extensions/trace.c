/*
 * trace extension module for crash
 *
 * Copyright (C) 2009, 2010 FUJITSU LIMITED
 * Author: Lai Jiangshan <laijs@cn.fujitsu.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 */

#define _GNU_SOURCE
#include "defs.h"
#include <stdio.h>
#include <ctype.h>
#include <setjmp.h>
#include <stdlib.h>

static int verbose = 0;

static int nr_cpu_ids;

/*
 * lockless ring_buffer and old non-lockless ring_buffer are both supported.
 */
static int lockless_ring_buffer;
static int per_cpu_buffer_sizes;

#define koffset(struct, member) struct##_##member##_offset

static int koffset(trace_array, buffer);
static int koffset(tracer, name);

static int koffset(ring_buffer, pages);
static int koffset(ring_buffer, flags);
static int koffset(ring_buffer, cpus);
static int koffset(ring_buffer, buffers);

static int koffset(ring_buffer_per_cpu, cpu);
static int koffset(ring_buffer_per_cpu, pages);
static int koffset(ring_buffer_per_cpu, nr_pages);
static int koffset(ring_buffer_per_cpu, head_page);
static int koffset(ring_buffer_per_cpu, tail_page);
static int koffset(ring_buffer_per_cpu, commit_page);
static int koffset(ring_buffer_per_cpu, reader_page);
static int koffset(ring_buffer_per_cpu, overrun);
static int koffset(ring_buffer_per_cpu, entries);

static int koffset(buffer_page, read);
static int koffset(buffer_page, list);
static int koffset(buffer_page, page);

static int koffset(list_head, next);

static int koffset(ftrace_event_call, list);

static int koffset(ftrace_event_field, link);
static int koffset(ftrace_event_field, name);
static int koffset(ftrace_event_field, type);
static int koffset(ftrace_event_field, offset);
static int koffset(ftrace_event_field, size);
static int koffset(ftrace_event_field, is_signed);

static int koffset(POINTER_SYM, POINTER) = 0;

struct ring_buffer_per_cpu {
	ulong kaddr;

	ulong head_page;
	ulong tail_page;
	ulong commit_page;
	ulong reader_page;
	ulong real_head_page;

	int head_page_index;
	unsigned int nr_pages;
	ulong *pages;

	ulong *linear_pages;
	int nr_linear_pages;

	ulong overrun;
	ulong entries;
};

static ulong global_trace;
static ulong global_ring_buffer;
static unsigned global_pages;
static struct ring_buffer_per_cpu *global_buffers;

static ulong max_tr_trace;
static ulong max_tr_ring_buffer;
static unsigned max_tr_pages;
static struct ring_buffer_per_cpu *max_tr_buffers;

static ulong ftrace_events;
static ulong current_trace;
static const char *current_tracer_name;

static void ftrace_destroy_event_types(void);
static int ftrace_init_event_types(void);

/* at = ((struct *)ptr)->member */
#define read_value(at, ptr, struct, member)				\
	do {								\
		if (!readmem(ptr + koffset(struct, member), KVADDR,	\
				&at, sizeof(at), #struct "'s " #member,	\
				RETURN_ON_ERROR))			\
			goto out_fail;\
	} while (0)

/* Remove the "const" qualifiers for ptr */
#define free(ptr) free((void *)(ptr))

static int write_and_check(int fd, void *data, size_t size)
{
	size_t tot = 0;
	size_t w;

	do {
		w = write(fd, data, size - tot);
		tot += w;

		if (w <= 0)
			return -1;
	} while (tot != size);

	return 0;
}

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int init_offsets(void)
{
#define init_offset(struct, member) do {				\
		koffset(struct, member) = MEMBER_OFFSET(#struct, #member);\
		if (koffset(struct, member) < 0) {			\
			fprintf(fp, "failed to init the offset, struct:"\
				#struct ", member:" #member);		\
			return -1;					\
		}							\
	} while (0)


	init_offset(trace_array, buffer);
	init_offset(tracer, name);

	if (MEMBER_EXISTS("ring_buffer_per_cpu", "nr_pages")) {
		per_cpu_buffer_sizes = 1;
		if (verbose)
			fprintf(fp, "per cpu buffer sizes\n");
	}

	if (!per_cpu_buffer_sizes)
		init_offset(ring_buffer, pages);
	init_offset(ring_buffer, flags);
	init_offset(ring_buffer, cpus);
	init_offset(ring_buffer, buffers);

	if (MEMBER_SIZE("ring_buffer_per_cpu", "pages") == sizeof(ulong)) {
		lockless_ring_buffer = 1;
		if (verbose)
			fprintf(fp, "lockless\n");
	}

	if (per_cpu_buffer_sizes)
		init_offset(ring_buffer_per_cpu, nr_pages);
	init_offset(ring_buffer_per_cpu, cpu);
	init_offset(ring_buffer_per_cpu, pages);
	init_offset(ring_buffer_per_cpu, head_page);
	init_offset(ring_buffer_per_cpu, tail_page);
	init_offset(ring_buffer_per_cpu, commit_page);
	init_offset(ring_buffer_per_cpu, reader_page);
	init_offset(ring_buffer_per_cpu, overrun);
	init_offset(ring_buffer_per_cpu, entries);

	init_offset(buffer_page, read);
	init_offset(buffer_page, list);
	init_offset(buffer_page, page);

	init_offset(list_head, next);

	init_offset(ftrace_event_call, list);

	init_offset(ftrace_event_field, link);
	init_offset(ftrace_event_field, name);
	init_offset(ftrace_event_field, type);
	init_offset(ftrace_event_field, offset);
	init_offset(ftrace_event_field, size);
	init_offset(ftrace_event_field, is_signed);

	return 0;
#undef init_offset
}

static void print_offsets(void)
{
	if (!verbose)
		return;

#define print_offset(struct, member) fprintf(fp,			\
	"koffset(" #struct ", " #member ") = %d\n", koffset(struct, member))

	print_offset(trace_array, buffer);
	print_offset(tracer, name);

	print_offset(ring_buffer, pages);
	print_offset(ring_buffer, flags);
	print_offset(ring_buffer, cpus);
	print_offset(ring_buffer, buffers);

	print_offset(ring_buffer_per_cpu, cpu);
	print_offset(ring_buffer_per_cpu, pages);
	print_offset(ring_buffer_per_cpu, head_page);
	print_offset(ring_buffer_per_cpu, tail_page);
	print_offset(ring_buffer_per_cpu, commit_page);
	print_offset(ring_buffer_per_cpu, reader_page);
	print_offset(ring_buffer_per_cpu, overrun);
	print_offset(ring_buffer_per_cpu, entries);

	print_offset(buffer_page, read);
	print_offset(buffer_page, list);
	print_offset(buffer_page, page);

	print_offset(list_head, next);

	print_offset(ftrace_event_call, list);

	print_offset(ftrace_event_field, link);
	print_offset(ftrace_event_field, name);
	print_offset(ftrace_event_field, type);
	print_offset(ftrace_event_field, offset);
	print_offset(ftrace_event_field, size);
	print_offset(ftrace_event_field, is_signed);
#undef print_offset
}

static int ftrace_init_pages(struct ring_buffer_per_cpu *cpu_buffer,
		unsigned nr_pages)
{
	unsigned j = 0, count = 0;
	ulong head, page;
	ulong real_head_page = cpu_buffer->head_page;

	cpu_buffer->pages = calloc(sizeof(ulong), nr_pages);
	if (cpu_buffer->pages == NULL)
		return -1;

	cpu_buffer->linear_pages = calloc(sizeof(ulong), nr_pages + 1);
	if (cpu_buffer->linear_pages == NULL) {
		return -1;
	}

	if (lockless_ring_buffer) {
		read_value(head, cpu_buffer->kaddr, ring_buffer_per_cpu, pages);
		cpu_buffer->pages[j++] = head - koffset(buffer_page, list);
	} else
		head = cpu_buffer->kaddr + koffset(ring_buffer_per_cpu, pages);

	page = head;
	for (;;) {
		read_value(page, page, list_head, next);
		if (page & 3) {
			/* lockless_ring_buffer */
			page &= ~3;
			real_head_page = page - koffset(buffer_page, list);
		}

		if (j == nr_pages)
			break;

		if (page == head) {
			error(INFO, "Num of pages is less than %d\n", nr_pages);
			goto out_fail;
		}

		cpu_buffer->pages[j++] = page - koffset(buffer_page, list);
	}

	if (page != head) {
		error(INFO, "Num of pages is larger than %d\n", nr_pages);
		goto out_fail;
	}

	/* find head page and head_page_index */

	cpu_buffer->real_head_page = real_head_page;
	cpu_buffer->head_page_index = -1;

	for (j = 0; j < nr_pages; j++) {
		if (cpu_buffer->pages[j] == real_head_page) {
			cpu_buffer->head_page_index = j;
			break;
		}
	}

	if (cpu_buffer->head_page_index == -1) {
		error(INFO, "error for resolve head_page_index\n");
		goto out_fail;
	}

	/* Setup linear pages */

	cpu_buffer->linear_pages[count++] = cpu_buffer->reader_page;

	if (cpu_buffer->reader_page == cpu_buffer->commit_page)
		goto done;

	j = cpu_buffer->head_page_index;
	for (;;) {
		cpu_buffer->linear_pages[count++] = cpu_buffer->pages[j];

		if (cpu_buffer->pages[j] == cpu_buffer->commit_page)
			break;

		j++;
		if (j == nr_pages)
			j = 0;

		if (j == cpu_buffer->head_page_index) {
			/* cpu_buffer->commit_page may be corrupted */
			break;
		}
	}

done:
	cpu_buffer->nr_linear_pages = count;

	return 0;

out_fail:
	return -1;
}

static void ftrace_destroy_buffers(struct ring_buffer_per_cpu *buffers)
{
	int i;

	for (i = 0; i < nr_cpu_ids; i++) {
		if (!buffers[i].kaddr)
			continue;

		free(buffers[i].pages);
		free(buffers[i].linear_pages);
	}
}

static int ftrace_init_buffers(struct ring_buffer_per_cpu *buffers,
		ulong ring_buffer, unsigned pages)
{
	int i;
	ulong buffers_array;

	read_value(buffers_array, ring_buffer, ring_buffer, buffers);

	for (i = 0; i < nr_cpu_ids; i++) {
		if (!readmem(buffers_array + sizeof(ulong) * i, KVADDR,
				&buffers[i].kaddr, sizeof(ulong),
				"ring_buffer's cpu buffer", RETURN_ON_ERROR))
			goto out_fail;

		if (!buffers[i].kaddr)
			continue;

#define buffer_read_value(member) read_value(buffers[i].member,		\
			buffers[i].kaddr, ring_buffer_per_cpu, member)

		buffer_read_value(head_page);
		buffer_read_value(tail_page);
		buffer_read_value(commit_page);
		buffer_read_value(reader_page);
		buffer_read_value(overrun);
		buffer_read_value(entries);
		if (per_cpu_buffer_sizes) {
			buffer_read_value(nr_pages);
			pages = buffers[i].nr_pages;
		} else
			buffers[i].nr_pages = pages;

#undef buffer_read_value

		if (ftrace_init_pages(buffers + i, pages) < 0)
			goto out_fail;

		if (verbose) {
			fprintf(fp, "overrun=%lu\n", buffers[i].overrun);
			fprintf(fp, "entries=%lu\n", buffers[i].entries);
		}
	}

	return 0;

out_fail:
	ftrace_destroy_buffers(buffers);
	return -1;
}

static int ftrace_int_global_trace(void)
{
	read_value(global_ring_buffer, global_trace, trace_array, buffer);
	read_value(global_pages, global_ring_buffer, ring_buffer, pages);

	global_buffers = calloc(sizeof(*global_buffers), nr_cpu_ids);
	if (global_buffers == NULL)
		goto out_fail;

	if (ftrace_init_buffers(global_buffers, global_ring_buffer,
			global_pages) < 0)
		goto out_fail;

	return 0;

out_fail:
	free(global_buffers);
	return -1;
}

static int ftrace_int_max_tr_trace(void)
{
	read_value(max_tr_ring_buffer, max_tr_trace, trace_array, buffer);

	if (!max_tr_ring_buffer)
		return 0;

	read_value(max_tr_pages, max_tr_ring_buffer, ring_buffer, pages);

	max_tr_buffers = calloc(sizeof(*max_tr_buffers), nr_cpu_ids);
	if (max_tr_buffers == NULL)
		goto out_fail;

	if (ftrace_init_buffers(max_tr_buffers, max_tr_ring_buffer,
			max_tr_pages) < 0)
		goto out_fail;

	return 0;

out_fail:
	free(max_tr_buffers);
	max_tr_ring_buffer = 0;
	return -1;
}

static int ftrace_init_current_tracer(void)
{
	ulong addr;
	char tmp[128];

	/* Get current tracer name */
	read_value(addr, current_trace, POINTER_SYM, POINTER);
	read_value(addr, addr, tracer, name);
	read_string(addr, tmp, 128);

	current_tracer_name = strdup(tmp);
	if (current_tracer_name == NULL)
		goto out_fail;

	return 0;

out_fail:
	return -1;
}

static int ftrace_init(void)
{
        struct syment *sym_global_trace;
	struct syment *sym_max_tr_trace;
	struct syment *sym_ftrace_events;
	struct syment *sym_current_trace;

	sym_global_trace = symbol_search("global_trace");
	sym_max_tr_trace = symbol_search("max_tr");
	sym_ftrace_events = symbol_search("ftrace_events");
	sym_current_trace = symbol_search("current_trace");

	if (sym_global_trace == NULL || sym_max_tr_trace == NULL
			|| sym_ftrace_events == NULL
			|| sym_current_trace == NULL)
		return -1;

	global_trace = sym_global_trace->value;
	max_tr_trace = sym_max_tr_trace->value;
	ftrace_events = sym_ftrace_events->value;
	current_trace = sym_current_trace->value;

	if (!try_get_symbol_data("nr_cpu_ids", sizeof(int), &nr_cpu_ids))
		nr_cpu_ids = 1;

	if (init_offsets() < 0)
		return -1;
	print_offsets();

	if (ftrace_int_global_trace() < 0)
		goto out_0;

	ftrace_int_max_tr_trace();

	if (ftrace_init_event_types() < 0)
		goto out_1;

	if (ftrace_init_current_tracer() < 0)
		goto out_2;

	return 0;

out_2:
	ftrace_destroy_event_types();
out_1:
	if (max_tr_ring_buffer) {
		ftrace_destroy_buffers(max_tr_buffers);
		free(max_tr_buffers);
	}
	ftrace_destroy_buffers(global_buffers);
	free(global_buffers);
out_0:
	return -1;
}

static void ftrace_destroy(void)
{
	free(current_tracer_name);
	ftrace_destroy_event_types();

	if (max_tr_ring_buffer) {
		ftrace_destroy_buffers(max_tr_buffers);
		free(max_tr_buffers);
	}

	ftrace_destroy_buffers(global_buffers);
	free(global_buffers);
}

static int ftrace_dump_page(int fd, ulong page, void *page_tmp)
{
	ulong raw_page;

	read_value(raw_page, page, buffer_page, page);

	if (!readmem(raw_page, KVADDR, page_tmp, PAGESIZE(), "get page context",
			RETURN_ON_ERROR))
		goto out_fail;

	if (write_and_check(fd, page_tmp, PAGESIZE()))
		return -1;

	return 0;

out_fail:
	return -1;
}

static
void ftrace_dump_buffer(int fd, struct ring_buffer_per_cpu *cpu_buffer,
		unsigned pages, void *page_tmp)
{
	int i;

	for (i = 0; i < cpu_buffer->nr_linear_pages; i++) {
		if (ftrace_dump_page(fd, cpu_buffer->linear_pages[i],
				page_tmp) < 0)
			break;
	}
}

static int try_mkdir(const char *pathname, mode_t mode)
{
	int ret;

	ret = mkdir(pathname, mode);
	if (ret < 0) {
		if (errno == EEXIST)
			return 0;

		error(INFO, "mkdir failed\n");
		return -1;
	}

	return 0;
}

static int ftrace_dump_buffers(const char *per_cpu_path)
{
	int i;
	void *page_tmp;
	char path[PATH_MAX];
	int fd;

	page_tmp = malloc(PAGESIZE());
	if (page_tmp == NULL)
		return -1;

	for (i = 0; i < nr_cpu_ids; i++) {
		struct ring_buffer_per_cpu *cpu_buffer = &global_buffers[i];

		if (!cpu_buffer->kaddr)
			continue;

		snprintf(path, sizeof(path), "%s/cpu%d", per_cpu_path, i);
		if (try_mkdir(path, 0755) < 0)
			goto out_fail;

		snprintf(path, sizeof(path), "%s/cpu%d/trace_pipe_raw",
				per_cpu_path, i);
		fd = open(path, O_WRONLY | O_CREAT, 0644);
		if (fd < 0)
			goto out_fail;

		ftrace_dump_buffer(fd, cpu_buffer, global_pages, page_tmp);
		close(fd);
	}

	free(page_tmp);
	return 0;

out_fail:
	free(page_tmp);
	return -1;
}

#define MAX_CACHE_ID	256

struct ftrace_field {
	const char *name;
	const char *type;
	int offset;
	int size;
	int is_signed;
};

struct event_type {
	struct event_type *next;
	const char *system;
	const char *name;
	int plugin;
	const char *print_fmt;
	int id;
	int nfields;
	struct ftrace_field *fields;
};

static struct event_type *event_type_cache[MAX_CACHE_ID];
static struct event_type **event_types;
static int nr_event_types;

static struct ftrace_field *ftrace_common_fields;
static int ftrace_common_fields_count;

static int syscall_get_enter_fields(ulong call, ulong *fields)
{
	static int inited;
	static int data_offset;
	static int enter_fields_offset;

	ulong metadata;

	if (inited)
		goto work;

	inited = 1;
	data_offset = MEMBER_OFFSET("ftrace_event_call", "data");
	if (data_offset < 0)
		return -1;

	enter_fields_offset = MEMBER_OFFSET("syscall_metadata", "enter_fields");
	if (enter_fields_offset < 0)
		return -1;

work:
	if (data_offset < 0 || enter_fields_offset < 0)
		return -1;

	if (!readmem(call + data_offset, KVADDR, &metadata, sizeof(metadata),
			"read ftrace_event_call data", RETURN_ON_ERROR))
		return -1;

	*fields = metadata + enter_fields_offset;
	return 0;
}

static int syscall_get_exit_fields_old(ulong call, ulong *fields)
{
	static int inited;
	static int data_offset;
	static int exit_fields_offset;

	ulong metadata;

	if (inited)
		goto work;

	inited = 1;
	data_offset = MEMBER_OFFSET("ftrace_event_call", "data");
	if (data_offset < 0)
		return -1;

	exit_fields_offset = MEMBER_OFFSET("syscall_metadata", "exit_fields");
	if (exit_fields_offset < 0)
		return -1;

work:
	if (data_offset < 0 || exit_fields_offset < 0)
		return -1;

	if (!readmem(call + data_offset, KVADDR, &metadata, sizeof(metadata),
			"read ftrace_event_call data", RETURN_ON_ERROR))
		return -1;

	*fields = metadata + exit_fields_offset;
	return 0;
}

static int syscall_get_exit_fields(ulong call, ulong *fields)
{
	static int inited;
	static ulong syscall_exit_fields_value;

	if (!inited) {
		struct syment *sp;

		if (!(sp = symbol_search("syscall_exit_fields"))) {
			inited = -1;
		} else {
			syscall_exit_fields_value = sp->value;
			inited = 1;
		}
	}

	if (inited == -1)
		return syscall_get_exit_fields_old(call, fields);

	*fields = syscall_exit_fields_value;

	return 0;
}

static
int ftrace_get_event_type_fields(ulong call, ulong *fields)
{
	static int inited;
	static int fields_offset;
	static int class_offset;
	static int get_fields_offset;
	static ulong syscall_get_enter_fields_value;
	static ulong syscall_get_exit_fields_value;

	struct syment *sp;
	ulong class, get_fields;

	if (inited)
		goto work;

	inited = 1;
	fields_offset = MEMBER_OFFSET("ftrace_event_call", "fields");

	class_offset = MEMBER_OFFSET("ftrace_event_call", "class");
	if (class_offset < 0)
		goto work;

	inited = 2;
	fields_offset = MEMBER_OFFSET("ftrace_event_class", "fields");
	if (fields_offset < 0)
		return -1;

	get_fields_offset = MEMBER_OFFSET("ftrace_event_class", "get_fields");
	if ((sp = symbol_search("syscall_get_enter_fields")) != NULL)
		syscall_get_enter_fields_value = sp->value;
	if ((sp = symbol_search("syscall_get_exit_fields")) != NULL)
		syscall_get_exit_fields_value = sp->value;

work:
	if (fields_offset < 0)
		return -1;

	if (inited == 1) {
		*fields = call + fields_offset;
		return 0;
	}

	if (!readmem(call + class_offset, KVADDR, &class, sizeof(class),
			"read ftrace_event_call class", RETURN_ON_ERROR))
		return -1;

	if (!readmem(class + get_fields_offset, KVADDR, &get_fields,
			sizeof(get_fields), "read ftrace_event_call get_fields",
			RETURN_ON_ERROR))
		return -1;

	if (!get_fields) {
		*fields = class + fields_offset;
		return 0;
	}

	if (get_fields == syscall_get_enter_fields_value)
		return syscall_get_enter_fields(call, fields);

	if (get_fields == syscall_get_exit_fields_value)
		return syscall_get_exit_fields(call, fields);

	fprintf(fp, "Unknown get_fields function\n");
	return -1;
}

static int ftrace_init_event_fields(ulong fields_head, int *pnfields,
		struct ftrace_field **pfields)
{
	ulong pos;

	int nfields = 0, max_fields = 16;
	struct ftrace_field *fields = NULL;

	read_value(pos, fields_head, list_head, next);

	if (pos == 0) {
		if (verbose)
			fprintf(fp, "no field, head: %lu\n", fields_head);
		return 0;
	}

	fields = malloc(sizeof(*fields) * max_fields);
	if (fields == NULL)
		return -1;

	while (pos != fields_head) {
		ulong field;
		ulong name_addr, type_addr;
		char field_name[128], field_type[128];
		int offset, size, is_signed;

		field = pos - koffset(ftrace_event_field, link);

		/* Read a field from the core */
		read_value(name_addr, field, ftrace_event_field, name);
		read_value(type_addr, field, ftrace_event_field, type);
		read_value(offset, field, ftrace_event_field, offset);
		read_value(size, field, ftrace_event_field, size);
		read_value(is_signed, field, ftrace_event_field, is_signed);

		if (!read_string(name_addr, field_name, 128))
			goto out_fail;
		if (!read_string(type_addr, field_type, 128))
			goto out_fail;

		/* Enlarge fields array when need */
		if (nfields >= max_fields) {
			void *tmp;

			max_fields = nfields * 2;
			tmp = realloc(fields, sizeof(*fields) * max_fields);
			if (tmp == NULL)
				goto out_fail;

			fields = tmp;
		}

		/* Set up and Add a field */
		fields[nfields].offset = offset;
		fields[nfields].size = size;
		fields[nfields].is_signed = is_signed;

		fields[nfields].name = strdup(field_name);
		if (fields[nfields].name == NULL)
			goto out_fail;

		fields[nfields].type = strdup(field_type);
		if (fields[nfields].type == NULL) {
			free(fields[nfields].name);
			goto out_fail;
		}

		nfields++;

		/* Advance to the next field */
		read_value(pos, pos, list_head, next);
	}

	*pnfields = nfields;
	*pfields = fields;

	return 0;

out_fail:
	for (nfields--; nfields >= 0; nfields--) {
		free(fields[nfields].name);
		free(fields[nfields].type);
	}

	free(fields);
	return -1;
}

static int ftrace_init_event_type(ulong call, struct event_type *aevent_type)
{
	ulong fields_head = 0;

	if (ftrace_get_event_type_fields(call, &fields_head) < 0)
		return -1;

	return ftrace_init_event_fields(fields_head, &aevent_type->nfields,
			&aevent_type->fields);
}

static int ftrace_init_common_fields(void)
{
	ulong ftrace_common_fields_head;
	struct syment *sp;

	sp = symbol_search("ftrace_common_fields");
	if (!sp)
		return 0;

	ftrace_common_fields_head = sp->value;

	return ftrace_init_event_fields(ftrace_common_fields_head,
			&ftrace_common_fields_count, &ftrace_common_fields);
}

static void ftrace_destroy_event_types(void)
{
	int i, j;

	for (i = 0; i < nr_event_types; i++) {
		for (j = 0; j < event_types[i]->nfields; j++) {
			free(event_types[i]->fields[j].name);
			free(event_types[i]->fields[j].type);
		}

		free(event_types[i]->fields);
		free(event_types[i]->system);
		free(event_types[i]->name);
		free(event_types[i]->print_fmt);
		free(event_types[i]);
	}

	free(event_types);
	free(ftrace_common_fields);
}

static
int ftrace_get_event_type_name(ulong call, char *name, int len)
{
	static int inited;
	static int name_offset;

	ulong name_addr;

	if (!inited) {
		inited = 1;
		name_offset = MEMBER_OFFSET("ftrace_event_call", "name");
	}

	if (name_offset < 0)
		return -1;

	if (!readmem(call + name_offset, KVADDR, &name_addr, sizeof(name_addr),
			"read ftrace_event_call name_addr", RETURN_ON_ERROR))
		return -1;

	if (!read_string(name_addr, name, len))
		return -1;

	return 0;
}

static
int ftrace_get_event_type_system(ulong call, char *system, int len)
{
	static int inited;
	static int sys_offset;
	static int class_offset;

	ulong ptr = call;
	ulong sys_addr;

	if (inited)
		goto work;

	inited = 1;
	sys_offset = MEMBER_OFFSET("ftrace_event_call", "system");

	if (sys_offset >= 0)
		goto work;

	class_offset = MEMBER_OFFSET("ftrace_event_call", "class");
	if (class_offset < 0)
		return -1;

	sys_offset = MEMBER_OFFSET("ftrace_event_class", "system");
	inited = 2;

work:
	if (sys_offset < 0)
		return -1;

	if (inited == 2 && !readmem(call + class_offset, KVADDR, &ptr,
			sizeof(ptr), "read ftrace_event_call class_addr",
			RETURN_ON_ERROR))
		return -1;

	if (!readmem(ptr + sys_offset, KVADDR, &sys_addr, sizeof(sys_addr),
			"read ftrace_event_call sys_addr", RETURN_ON_ERROR))
		return -1;

	if (!read_string(sys_addr, system, len))
		return -1;

	return 0;
}

static int read_long_string(ulong kvaddr, char **buf)
{
	char strbuf[MIN_PAGE_SIZE], *ret_buf = NULL;
	ulong kp;
	int cnt1, cnt2, size;

again:
	kp = kvaddr;
	size = 0;

	for (;;) {
		cnt1 = MIN_PAGE_SIZE - (kp & (MIN_PAGE_SIZE-1));

		if (!readmem(kp, KVADDR, strbuf, cnt1,
		    "readstring characters", QUIET|RETURN_ON_ERROR))
			return -1;

		cnt2 = strnlen(strbuf, cnt1);
		if (ret_buf)
			memcpy(ret_buf + size, strbuf, cnt2);
		kp += cnt2;
		size += cnt2;

		if (cnt2 < cnt1) {
			if (ret_buf) {
				break;
			} else {
				ret_buf = malloc(size + 1);
				if (!ret_buf)
					return -1;
				goto again;
			}
		}
	}

	ret_buf[size] = '\0';
	*buf = ret_buf;
	return size;
}

static
int ftrace_get_event_type_print_fmt(ulong call, char **print_fmt)
{
	static int inited;
	static int fmt_offset;

	ulong fmt_addr;

	if (!inited) {
		inited = 1;
		fmt_offset = MEMBER_OFFSET("ftrace_event_call", "print_fmt");
	}

	if (fmt_offset < 0) {
		*print_fmt = strdup("Unknown print_fmt");
		return 0;
	}

	if (!readmem(call + fmt_offset, KVADDR, &fmt_addr, sizeof(fmt_addr),
			"read ftrace_event_call fmt_addr", RETURN_ON_ERROR))
		return -1;

	return read_long_string(fmt_addr, print_fmt);
}

static
int ftrace_get_event_type_id(ulong call, int *id)
{
	static int inited;
	static int id_offset;

	if (!inited) {
		inited = 1;
		id_offset = MEMBER_OFFSET("ftrace_event_call", "id");

		if (id_offset < 0) {
			/* id = call->event.type */
			int f1 = MEMBER_OFFSET("ftrace_event_call", "event");
			int f2 = MEMBER_OFFSET("trace_event", "type");

			if (f1 >= 0 && f2 >= 0)
				id_offset = f1 + f2;
		}
	}

	if (id_offset < 0)
		return -1;

	if (!readmem(call + id_offset, KVADDR, id, sizeof(*id),
			"read ftrace_event_call id", RETURN_ON_ERROR))
		return -1;

	return 0;
}

static int ftrace_init_event_types(void)
{
	ulong event;
	struct event_type *aevent_type;
	int max_types = 128;

	event_types = malloc(sizeof(*event_types) * max_types);
	if (event_types == NULL)
		return -1;

	read_value(event, ftrace_events, list_head, next);
	while (event != ftrace_events) {
		ulong call;
		char name[128], system[128], *print_fmt;
		int id;

		call = event - koffset(ftrace_event_call, list);

		/* Read a event type from the core */
		if (ftrace_get_event_type_id(call, &id) < 0 ||
		    ftrace_get_event_type_name(call, name, 128) < 0 ||
		    ftrace_get_event_type_system(call, system, 128) < 0 ||
		    ftrace_get_event_type_print_fmt(call, &print_fmt) < 0)
			goto out_fail;

		/* Enlarge event types array when need */
		if (nr_event_types >= max_types) {
			void *tmp;

			max_types = 2 * nr_event_types;
			tmp = realloc(event_types,
					sizeof(*event_types) * max_types);
			if (tmp == NULL) {
				free(print_fmt);
				goto out_fail;
			}

			event_types = tmp;
		}

		/* Create a event type */
		aevent_type = malloc(sizeof(*aevent_type));
		if (aevent_type == NULL) {
			free(print_fmt);
			goto out_fail;
		}

		aevent_type->system = strdup(system);
		aevent_type->name = strdup(name);
		aevent_type->print_fmt = print_fmt;
		aevent_type->id = id;
		aevent_type->nfields = 0;
		aevent_type->fields = NULL;

		if (aevent_type->system == NULL || aevent_type->name == NULL)
			goto out_fail_free_aevent_type;

		if (ftrace_init_event_type(call, aevent_type) < 0)
			goto out_fail_free_aevent_type;

		if (!strcmp("ftrace", aevent_type->system))
			aevent_type->plugin = 1;
		else
			aevent_type->plugin = 0;

		/* Add a event type */
		event_types[nr_event_types++] = aevent_type;
		if ((unsigned)id < MAX_CACHE_ID)
			event_type_cache[id] = aevent_type;

		/* Advance to the next event type */
		read_value(event, event, list_head, next);
	}

	if (ftrace_init_common_fields() < 0)
		goto out_fail;

	return 0;

out_fail_free_aevent_type:
	free(aevent_type->system);
	free(aevent_type->name);
	free(aevent_type->print_fmt);
	free(aevent_type);
out_fail:
	ftrace_destroy_event_types();
	return -1;
}

#define default_common_field_count 5

static int ftrace_dump_event_type(struct event_type *t, const char *path)
{
	char format_path[PATH_MAX];
	FILE *out;
	int i, nfields;
	struct ftrace_field *fields;
	int printed_common_field = 0;

	snprintf(format_path, sizeof(format_path), "%s/format", path);
	out = fopen(format_path, "w");
	if (out == NULL)
		return -1;

	fprintf(out, "name: %s\n", t->name);
	fprintf(out, "ID: %d\n", t->id);
	fprintf(out, "format:\n");

	if (ftrace_common_fields_count) {
		nfields = ftrace_common_fields_count;
		fields = ftrace_common_fields;
	} else {
		nfields = default_common_field_count;
		fields = &t->fields[t->nfields - nfields];
	}

again:
	for (i = nfields - 1; i >= 0; i--) {
		/*
		 * Smartly shows the array type(except dynamic array).
		 * Normal:
		 *	field:TYPE VAR
		 * If TYPE := TYPE[LEN], it is shown:
		 *	field:TYPE VAR[LEN]
		 */
		struct ftrace_field *field = &fields[i];
		const char *array_descriptor = strchr(field->type, '[');

		if (!strncmp(field->type, "__data_loc", 10))
			array_descriptor = NULL;

		if (!array_descriptor) {
			fprintf(out, "\tfield:%s %s;\toffset:%u;"
					"\tsize:%u;\tsigned:%d;\n",
					field->type, field->name, field->offset,
					field->size, !!field->is_signed);
		} else {
			fprintf(out, "\tfield:%.*s %s%s;\toffset:%u;"
					"\tsize:%u;\tsigned:%d;\n",
					(int)(array_descriptor - field->type),
					field->type, field->name,
					array_descriptor, field->offset,
					field->size, !!field->is_signed);
		}
	}

	if (!printed_common_field) {
		fprintf(out, "\n");

		if (ftrace_common_fields_count)
			nfields = t->nfields;
		else
			nfields = t->nfields - default_common_field_count;
		fields = t->fields;

		printed_common_field = 1;
		goto again;
	}

	fprintf(out, "\nprint fmt: %s\n", t->print_fmt);

	fclose(out);

	return 0;
}

static int ftrace_dump_event_types(const char *events_path)
{
	int i;

	for (i = 0; i < nr_event_types; i++) {
		char path[PATH_MAX];
		struct event_type *t = event_types[i];

		snprintf(path, sizeof(path), "%s/%s", events_path, t->system);
		if (try_mkdir(path, 0755) < 0)
			return -1;

		snprintf(path, sizeof(path), "%s/%s/%s", events_path,
			t->system, t->name);
		if (try_mkdir(path, 0755) < 0)
			return -1;

		if (ftrace_dump_event_type(t, path) < 0)
			return -1;
	}

	return 0;
}

static void show_basic_info(void)
{
	fprintf(fp, "current tracer is %s\n", current_tracer_name);
}

static int dump_saved_cmdlines(const char *dump_tracing_dir)
{
	char path[PATH_MAX];
	FILE *out;
	int i;
	struct task_context *tc;

	snprintf(path, sizeof(path), "%s/saved_cmdlines", dump_tracing_dir);
	out = fopen(path, "w");
	if (out == NULL)
		return -1;

	tc = FIRST_CONTEXT();
        for (i = 0; i < RUNNING_TASKS(); i++)
		fprintf(out, "%d %s\n", (int)tc[i].pid, tc[i].comm);

	fclose(out);
	return 0;
}

static int dump_kallsyms(const char *dump_tracing_dir)
{
	char path[PATH_MAX];
	FILE *out;
	int i;
	struct syment *sp;

	snprintf(path, sizeof(path), "%s/kallsyms", dump_tracing_dir);
	out = fopen(path, "w");
	if (out == NULL)
		return -1;

	for (sp = st->symtable; sp < st->symend; sp++)
		fprintf(out, "%lx %c %s\n", sp->value, sp->type, sp->name);

	for (i = 0; i < st->mods_installed; i++) {
		struct load_module *lm = &st->load_modules[i];

		for (sp = lm->mod_symtable; sp <= lm->mod_symend; sp++) {
			if (!strncmp(sp->name, "_MODULE_", strlen("_MODULE_")))
				continue;

			fprintf(out, "%lx %c %s\t[%s]\n", sp->value, sp->type,
					sp->name, lm->mod_name);
		}
	}

	fclose(out);
	return 0;
}

static int trace_cmd_data_output(int fd);

static void ftrace_dump(int argc, char *argv[])
{
	int c;
	int dump_meta_data = 0;
	int dump_symbols = 0;
	char *dump_tracing_dir;
	char path[PATH_MAX];
	int ret;

        while ((c = getopt(argc, argv, "smt")) != EOF) {
                switch(c)
		{
		case 's':
			dump_symbols = 1;
			break;
		case 'm':
			dump_meta_data = 1;
			break;
		case 't':
			if (dump_symbols || dump_meta_data || argc - optind > 1)
				cmd_usage(pc->curcmd, SYNOPSIS);
			else {
				char *trace_dat = "trace.dat";
				int fd;

				if (argc - optind == 0)
					trace_dat = "trace.dat";
				else if (argc - optind == 1)
					trace_dat = argv[optind];
				fd = open(trace_dat, O_WRONLY | O_CREAT
						| O_TRUNC, 0644);
				trace_cmd_data_output(fd);
				close(fd);
			}
			return;
		default:
			cmd_usage(pc->curcmd, SYNOPSIS);
			return;
		}
	}

	if (argc - optind == 0) {
		dump_tracing_dir = "dump_tracing_dir";
	} else if (argc - optind == 1) {
		dump_tracing_dir = argv[optind];
	} else {
		cmd_usage(pc->curcmd, SYNOPSIS);
		return;
	}

	ret = mkdir(dump_tracing_dir, 0755);
	if (ret < 0) {
		if (errno == EEXIST)
			error(INFO, "mkdir: %s exists\n", dump_tracing_dir);
		return;
	}

	snprintf(path, sizeof(path), "%s/per_cpu", dump_tracing_dir);
	if (try_mkdir(path, 0755) < 0)
		return;

	if (ftrace_dump_buffers(path) < 0)
		return;

	if (dump_meta_data) {
		/* Dump event types */
		snprintf(path, sizeof(path), "%s/events", dump_tracing_dir);
		if (try_mkdir(path, 0755) < 0)
			return;

		if (ftrace_dump_event_types(path) < 0)
			return;

		/* Dump pids with corresponding cmdlines */
		if (dump_saved_cmdlines(dump_tracing_dir) < 0)
			return;
	}

	if (dump_symbols) {
		/* Dump all symbols of the kernel */
		dump_kallsyms(dump_tracing_dir);
	}
}

static void ftrace_show(int argc, char *argv[])
{
	char buf[4096];
	char tmp[] = "/tmp/crash.trace_dat.XXXXXX";
	char *trace_cmd = "trace-cmd", *env_trace_cmd = getenv("TRACE_CMD");
	int fd;
	FILE *file;
	size_t ret;
	size_t nitems __attribute__ ((__unused__));
	char *unused __attribute__ ((__unused__));

	/* check trace-cmd */
	if (env_trace_cmd)
		trace_cmd = env_trace_cmd;
	buf[0] = 0;
	if ((file = popen(trace_cmd, "r"))) {
		ret = fread(buf, 1, sizeof(buf), file);
		buf[ret] = 0;
	}
	if (!strstr(buf, "trace-cmd version")) {
		if (env_trace_cmd)
			fprintf(fp, "Invalid environment TRACE_CMD: %s\n",
					env_trace_cmd);
		else
			fprintf(fp, "\"trace show\" requires trace-cmd.\n"
					"please set the environment TRACE_CMD "
					"if you installed it in a special path\n"
					);
		return;
	}

	/* dump trace.dat to the temp file */
	unused = mktemp(tmp);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (trace_cmd_data_output(fd) < 0)
		goto out;

	/* splice the output of trace-cmd to user */
	snprintf(buf, sizeof(buf), "%s report %s", trace_cmd, tmp);
	if (!(file = popen(buf, "r")))
		goto out;
	for (;;) {
		ret = fread(buf, 1, sizeof(buf), file);
		if (ret == 0)
			break;
		nitems = fwrite(buf, 1, ret, fp);
	}
	pclose(file);
out:
	close(fd);
	unlink(tmp);
	return;
}

static void cmd_ftrace(void)
{
	if (argcnt == 1)
		show_basic_info();
	else if (!strcmp(args[1], "dump"))
		ftrace_dump(argcnt - 1, args + 1);
	else if (!strcmp(args[1], "show"))
		ftrace_show(argcnt - 1, args + 1);
	else if (!strcmp(args[1], "report"))
		ftrace_show(argcnt - 1, args + 1);
	else
		cmd_usage(pc->curcmd, SYNOPSIS);
}

static char *help_ftrace[] = {
"trace",
"show or dump the tracing info",
"[ <show [-c <cpulist>] [-f [no]<flagname>]> | <dump [-sm] <dest-dir>> ]",
"trace",
"    shows the current tracer and other informations.",
"",
"trace show",
"    shows all events with readability text(sorted by timestamp)",
"",
"trace report",
"    the same as \"trace show\"",
"",
"trace dump [-sm] <dest-dir>",
"    dump ring_buffers to dest-dir. Then you can parse it",
"    by other tracing tools. The dirs and files are generated",
"    the same as debugfs/tracing.",
"    -m: also dump metadata of ftrace.",
"    -s: also dump symbols of the kernel.",
"trace dump -t [output-file-name]",
"   dump ring_buffers and all meta data to a file that can",
"   be parsed by trace-cmd. Default output file name is \"trace.dat\".",
NULL
};

static struct command_table_entry command_table[] = {
	{ "trace", cmd_ftrace, help_ftrace, 0 },
	{ NULL, 0, 0, 0 }
};

static int ftrace_initialized;

void __attribute__((constructor))
trace_init(void)
{
	if (ftrace_init() < 0)
		return;

	ftrace_initialized = 1;
	register_extension(command_table);
}

void __attribute__((destructor))
trace_fini(void)
{
	if (ftrace_initialized)
		ftrace_destroy();
}

#define TRACE_CMD_FILE_VERSION_STRING "6"

static inline int host_bigendian(void)
{
	unsigned char str[] = { 0x1, 0x2, 0x3, 0x4 };
	unsigned int *ptr;

	ptr = (unsigned int *)str;
	return *ptr == 0x01020304;
}

static char *tmp_file_buf;
static unsigned long long tmp_file_pos;
static unsigned long long tmp_file_size;
static int tmp_file_error;

static int init_tmp_file(void)
{
	tmp_file_buf = malloc(4096);
	if (tmp_file_buf == NULL)
		return -1;

	tmp_file_pos = 0;
	tmp_file_size = 4096;
	tmp_file_error = 0;

	return 0;
}

static void destory_tmp_file(void)
{
	free(tmp_file_buf);
}

#define tmp_fprintf(fmt...)						\
do {									\
	char *__buf = tmp_file_buf;					\
	unsigned long long __pos;					\
									\
	if (tmp_file_error)						\
		break;							\
	__pos = tmp_file_pos;						\
	__pos += snprintf(__buf + __pos, tmp_file_size - __pos, fmt);	\
	if (__pos >= tmp_file_size) {					\
		tmp_file_size = __pos + tmp_file_size;			\
		__buf = realloc(__buf, tmp_file_size);			\
		if (!__buf) {						\
			tmp_file_error = 1;				\
			break;						\
		}							\
		tmp_file_buf = __buf;					\
		__pos = tmp_file_pos;					\
		__pos += snprintf(__buf + __pos, tmp_file_size - __pos, fmt);\
	}								\
	tmp_file_pos = __pos;						\
} while (0)

static int tmp_file_record_size4(int fd)
{
	unsigned int size = tmp_file_pos;

	if (tmp_file_error)
		return -1;
	if (write_and_check(fd, &size, 4))
		return -1;
	return 0;
}

static int tmp_file_record_size8(int fd)
{
	if (tmp_file_error)
		return -1;
	if (write_and_check(fd, &tmp_file_pos, 8))
		return -1;
	return 0;
}

static int tmp_file_flush(int fd)
{
	if (tmp_file_error)
		return -1;
	if (write_and_check(fd, tmp_file_buf, tmp_file_pos))
		return -1;
	tmp_file_pos = 0;
	return 0;
}

static int save_initial_data(int fd)
{
	int page_size;
	char buf[20];

	if (write_and_check(fd, "\027\010\104tracing", 10))
		return -1;

	if (write_and_check(fd, TRACE_CMD_FILE_VERSION_STRING,
				strlen(TRACE_CMD_FILE_VERSION_STRING) + 1))
		return -1;

	/* Crash ensure core file endian and the host endian are the same */
	if (host_bigendian())
		buf[0] = 1;
	else
		buf[0] = 0;

	if (write_and_check(fd, buf, 1))
		return -1;

	/* save size of long (this may not be what the kernel is) */
	buf[0] = sizeof(long);
	if (write_and_check(fd, buf, 1))
		return -1;

	page_size = PAGESIZE();
	if (write_and_check(fd, &page_size, 4))
		return -1;

	return 0;
}

static int save_header_files(int fd)
{
	/* save header_page */
	if (write_and_check(fd, "header_page", 12))
		return -1;

	tmp_fprintf("\tfield: u64 timestamp;\toffset:0;\tsize:8;\tsigned:0;\n");

	tmp_fprintf("\tfield: local_t commit;\toffset:8;\tsize:%u;\t"
			"signed:1;\n", (unsigned int)sizeof(long));

	tmp_fprintf("\tfield: int overwrite;\toffset:8;\tsize:%u;\tsigned:1;\n",
			(unsigned int)sizeof(long));

	tmp_fprintf("\tfield: char data;\toffset:%u;\tsize:%u;\tsigned:1;\n",
			(unsigned int)(8 + sizeof(long)),
			(unsigned int)(PAGESIZE() - 8 - sizeof(long)));

	if (tmp_file_record_size8(fd))
		return -1;
	if (tmp_file_flush(fd))
		return -1;

	/* save header_event */
	if (write_and_check(fd, "header_event", 13))
		return -1;

	tmp_fprintf(
			"# compressed entry header\n"
			"\ttype_len    :    5 bits\n"
			"\ttime_delta  :   27 bits\n"
			"\tarray       :   32 bits\n"
			"\n"
			"\tpadding     : type == 29\n"
			"\ttime_extend : type == 30\n"
			"\tdata max type_len  == 28\n"
	);

	if (tmp_file_record_size8(fd))
		return -1;
	if (tmp_file_flush(fd))
		return -1;

	return 0;
}

static int save_event_file(int fd, struct event_type *t)
{
	int i, nfields;
	struct ftrace_field *fields;
	int printed_common_field = 0;

	tmp_fprintf("name: %s\n", t->name);
	tmp_fprintf("ID: %d\n", t->id);
	tmp_fprintf("format:\n");

	if (ftrace_common_fields_count) {
		nfields = ftrace_common_fields_count;
		fields = ftrace_common_fields;
	} else {
		nfields = default_common_field_count;
		fields = &t->fields[t->nfields - nfields];
	}

again:
	for (i = nfields - 1; i >= 0; i--) {
		/*
		 * Smartly shows the array type(except dynamic array).
		 * Normal:
		 *	field:TYPE VAR
		 * If TYPE := TYPE[LEN], it is shown:
		 *	field:TYPE VAR[LEN]
		 */
		struct ftrace_field *field = &fields[i];
		const char *array_descriptor = strchr(field->type, '[');

		if (!strncmp(field->type, "__data_loc", 10))
			array_descriptor = NULL;

		if (!array_descriptor) {
			tmp_fprintf("\tfield:%s %s;\toffset:%u;"
					"\tsize:%u;\tsigned:%d;\n",
					field->type, field->name, field->offset,
					field->size, !!field->is_signed);
		} else {
			tmp_fprintf("\tfield:%.*s %s%s;\toffset:%u;"
					"\tsize:%u;\tsigned:%d;\n",
					(int)(array_descriptor - field->type),
					field->type, field->name,
					array_descriptor, field->offset,
					field->size, !!field->is_signed);
		}
	}

	if (!printed_common_field) {
		tmp_fprintf("\n");

		if (ftrace_common_fields_count)
			nfields = t->nfields;
		else
			nfields = t->nfields - default_common_field_count;
		fields = t->fields;

		printed_common_field = 1;
		goto again;
	}

	tmp_fprintf("\nprint fmt: %s\n", t->print_fmt);

	if (tmp_file_record_size8(fd))
		return -1;
	return tmp_file_flush(fd);
}

static int save_system_files(int fd, int *system_ids, int system_id)
{
	int i, total = 0;

	for (i = 0; i < nr_event_types; i++) {
		if (system_ids[i] == system_id)
			total++;
	}

	if (write_and_check(fd, &total, 4))
		return -1;

	for (i = 0; i < nr_event_types; i++) {
		if (system_ids[i] != system_id)
			continue;

		if (save_event_file(fd, event_types[i]))
			return -1;
	}

	return 0;
}

static int save_events_files(int fd)
{
	int system_id = 1, *system_ids;
	const char *system = "ftrace";
	int i;
	int nr_systems;

	system_ids = calloc(sizeof(*system_ids), nr_event_types);
	if (system_ids == NULL)
		return -1;

	for (;;) {
		for (i = 0; i < nr_event_types; i++) {
			if (system_ids[i])
				continue;
			if (!system) {
				system = event_types[i]->system;
				system_ids[i] = system_id;
				continue;
			}
			if (!strcmp(event_types[i]->system, system))
				system_ids[i] = system_id;
		}
		if (!system)
			break;
		system_id++;
		system = NULL;
	}

	/* ftrace events */
	if (save_system_files(fd, system_ids, 1))
		goto fail;

	/* other systems events */
	nr_systems = system_id - 2;
	if (write_and_check(fd, &nr_systems, 4))
		goto fail;
	for (system_id = 2; system_id < nr_systems + 2; system_id++) {
		for (i = 0; i < nr_event_types; i++) {
			if (system_ids[i] == system_id)
				break;
		}
		if (write_and_check(fd, (void *)event_types[i]->system,
				strlen(event_types[i]->system) + 1))
			goto fail;
		if (save_system_files(fd, system_ids, system_id))
			goto fail;
	}

	free(system_ids);
	return 0;

fail:
	free(system_ids);
	return -1;
}

static int save_proc_kallsyms(int fd)
{
	int i;
	struct syment *sp;

	for (sp = st->symtable; sp < st->symend; sp++)
		tmp_fprintf("%lx %c %s\n", sp->value, sp->type, sp->name);

	for (i = 0; i < st->mods_installed; i++) {
		struct load_module *lm = &st->load_modules[i];

		for (sp = lm->mod_symtable; sp <= lm->mod_symend; sp++) {
			if (!strncmp(sp->name, "_MODULE_", strlen("_MODULE_")))
				continue;

			tmp_fprintf("%lx %c %s\t[%s]\n", sp->value, sp->type,
					sp->name, lm->mod_name);
		}
	}

	if (tmp_file_record_size4(fd))
		return -1;
	return tmp_file_flush(fd);
}

static int add_print_address(long address)
{
	char string[4096];
	size_t len;
	int i;

	len = read_string(address, string, sizeof(string));
	if (!len)
		return -1;

	tmp_fprintf("0x%lx : \"", address);

	for (i = 0; string[i]; i++) {
		switch (string[i]) {
		case '\n':
			tmp_fprintf("\\n");
			break;
		case '\t':
			tmp_fprintf("\\t");
			break;
		case '\\':
			tmp_fprintf("\\\\");
			break;
		case '"':
			tmp_fprintf("\\\"");
			break;
		default:
			tmp_fprintf("%c", string[i]);
		}
	}
	tmp_fprintf("\"\n");

	return 0;
}

static int save_ftrace_printk(int fd)
{
	struct kernel_list_head *mod_fmt;
	struct syment *s, *e, *b;
	long bprintk_fmt_s, bprintk_fmt_e;
	long *address;
	size_t i, count;
	int addr_is_array = 0;

	s = symbol_search("__start___trace_bprintk_fmt");
	e = symbol_search("__stop___trace_bprintk_fmt");
	if (s == NULL || e == NULL)
		return -1;

	bprintk_fmt_s = s->value;
	bprintk_fmt_e = e->value;
	count = (bprintk_fmt_e - bprintk_fmt_s) / sizeof(long);

	if (count == 0)
		goto do_mods;

	address = malloc(count * sizeof(long));
	if (address == NULL)
		return -1;

	if (!readmem(bprintk_fmt_s, KVADDR, address, count * sizeof(long),
			"get printk address", RETURN_ON_ERROR)) {
		free(address);
		return -1;
	}

	for (i = 0; i < count; i++) {
		if (add_print_address(address[i]) < 0) {
			free(address);
			return -1;
		}
	}

	free(address);

 do_mods:

	/* Add modules */
	b = symbol_search("trace_bprintk_fmt_list");
	if (!b)
		goto out;

	switch (MEMBER_TYPE("trace_bprintk_fmt", "fmt")) {
	case TYPE_CODE_ARRAY:
		addr_is_array = 1;
		break;
	case TYPE_CODE_PTR:
	default:
		/* default not array */
		break;
	}

	mod_fmt = (struct kernel_list_head *)GETBUF(SIZE(list_head));
	if (!readmem(b->value, KVADDR, mod_fmt,
		     SIZE(list_head), "trace_bprintk_fmt_list contents",
		     RETURN_ON_ERROR))
		goto out_free;

	while ((unsigned long)mod_fmt->next != b->value) {
		unsigned long addr;

		addr = (unsigned long)mod_fmt->next + SIZE(list_head);
		if (!addr_is_array) {
			if (!readmem(addr, KVADDR, &addr, sizeof(addr),
				     "trace_bprintk_fmt_list fmt field",
				     RETURN_ON_ERROR))
				goto out_free;
		}

		if (!readmem((unsigned long)mod_fmt->next, KVADDR, mod_fmt,
			     SIZE(list_head), "trace_bprintk_fmt_list contents",
			     RETURN_ON_ERROR))
			goto out_free;

		if (add_print_address(addr) < 0)
			goto out_free;
		count++;
	}

 out_free:
	FREEBUF(mod_fmt);
 out:
	if (count == 0) {
		unsigned int size = 0;
		return write_and_check(fd, &size, 4);
	}
	if (tmp_file_record_size4(fd))
		return -1;
	return tmp_file_flush(fd);
}

static int save_ftrace_cmdlines(int fd)
{
	int i;
	struct task_context *tc = FIRST_CONTEXT();

	for (i = 0; i < RUNNING_TASKS(); i++)
		tmp_fprintf("%d %s\n", (int)tc[i].pid, tc[i].comm);

	if (tmp_file_record_size8(fd))
		return -1;
	return tmp_file_flush(fd);
}

static int save_res_data(int fd, int nr_cpu_buffers)
{
	unsigned short option = 0;

	if (write_and_check(fd, &nr_cpu_buffers, 4))
		return -1;

	if (write_and_check(fd, "options  ", 10))
		return -1;

	if (write_and_check(fd, &option, 2))
		return -1;

	if (write_and_check(fd, "flyrecord", 10))
		return -1;

	return 0;
}

static int save_record_data(int fd, int nr_cpu_buffers)
{
	int i, j;
	unsigned long long offset, buffer_offset;
	void *page_tmp;

	offset = lseek(fd, 0, SEEK_CUR);
	offset += nr_cpu_buffers * 16;
	offset = (offset + (PAGESIZE() - 1)) & ~(PAGESIZE() - 1);
	buffer_offset = offset;

	for (i = 0; i < nr_cpu_ids; i++) {
		struct ring_buffer_per_cpu *cpu_buffer = &global_buffers[i];
		unsigned long long buffer_size;

		if (!cpu_buffer->kaddr)
			continue;

		buffer_size = PAGESIZE() * cpu_buffer->nr_linear_pages;
		if (write_and_check(fd, &buffer_offset, 8))
			return -1;
		if (write_and_check(fd, &buffer_size, 8))
			return -1;
		buffer_offset += buffer_size;
	}

	page_tmp = malloc(PAGESIZE());
	if (page_tmp == NULL)
		return -1;

	lseek(fd, offset, SEEK_SET);
	for (i = 0; i < nr_cpu_ids; i++) {
		struct ring_buffer_per_cpu *cpu_buffer = &global_buffers[i];

		if (!cpu_buffer->kaddr)
			continue;

		for (j = 0; j < cpu_buffer->nr_linear_pages; j++) {
			if (ftrace_dump_page(fd, cpu_buffer->linear_pages[j],
					page_tmp) < 0) {
				free(page_tmp);
				return -1;
			}
		}
	}

	free(page_tmp);

	return 0;
}

static int __trace_cmd_data_output(int fd)
{
	int i;
	int nr_cpu_buffers = 0;

	for (i = 0; i < nr_cpu_ids; i++) {
		struct ring_buffer_per_cpu *cpu_buffer = &global_buffers[i];

		if (!cpu_buffer->kaddr)
			continue;

		nr_cpu_buffers++;
	}

	if (save_initial_data(fd))
		return -1;
	if (save_header_files(fd))
		return -1;
	if (save_events_files(fd)) /* ftrace events and other systems events */
		return -1;
	if (save_proc_kallsyms(fd))
		return -1;
	if (save_ftrace_printk(fd))
		return -1;
	if (save_ftrace_cmdlines(fd))
		return -1;
	if (save_res_data(fd, nr_cpu_buffers))
		return -1;
	if (save_record_data(fd, nr_cpu_buffers))
		return -1;

	return 0;
}

static int trace_cmd_data_output(int fd)
{
	int ret;

	if (init_tmp_file())
		return -1;

	ret = __trace_cmd_data_output(fd);
	destory_tmp_file();

	return ret;
}
