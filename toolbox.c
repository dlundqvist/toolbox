/* :ts=2 */
#include <devices/scsidisk.h>

#include <exec/exec.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "toolbox_version.h"

static UBYTE versiontag[] = VERSTAG;

static struct RDArgs *args;

#define ARGS_TEMPLATE "DEVICE,UNIT/N,LD=LISTDEVICES/S,LF=LISTFILES/S," \
			"LCD=LISTCDS/S,SCD=SETCD/N,GET,CF=COUNTFILES/S,O=ODD/S,S=SHORT/S"

enum {
	ARG_DEVICE,
	ARG_UNIT,
	ARG_LD,
	ARG_LF,
	ARG_LCD,
	ARG_SCD,
	ARG_GET,
	ARG_CF,
	ARG_ODD,
	ARG_SHORT,
	ARG_NARG,
};
static LONG argsarray[ARG_NARG];

static const char *device = "scsi.device";
static LONG unit = 2;

static struct MsgPort *msgport;
static struct IOStdReq *ior;

__aligned union {
	struct toolbox_file {
		UBYTE index;
		UBYTE isdir;
		char  name[33];
		ULONG size;
	} files[100];
	UBYTE data[4096];
} data;

static UBYTE *datamem;

static UBYTE command[10];

static BPTR file;

static const char * const devicetypes[] = {
	"Fixed",
	"Removable",
	"Optical",
	"Floppy",
	"Magneto-Optical",
	"Sequential",
	"Network",
	"ZIP100",
};

struct rawdofmt_data {
	char *b;
};

static void __asm rawdofmt_putchbuffer(register __d0 char c,
									   register __a3 struct rawdofmt_data *d) {
	*d->b++ = c;
}

static void tprintf(STRPTR fmt, ...) {
	__aligned static char buffer[1025];
	struct rawdofmt_data d = {buffer};
	va_list ap;
	int len;
	va_start(ap, fmt);
	RawDoFmt(fmt, (APTR)ap, rawdofmt_putchbuffer, (APTR)&d);
	va_end(ap);
	len = d.b - buffer;
	if (len) {
		Write(Output(), buffer, len - 1);
	}
}

#define LIST_HEAD_INITIALIZER(l) {	\
	(struct Node *)&(l).lh_Tail,	\
	NULL,						  	\
	(struct Node *)&(l)				\
}

static struct List bstrs = LIST_HEAD_INITIALIZER(bstrs);
static struct List cstrs = LIST_HEAD_INITIALIZER(cstrs);

struct bstr_node {
	struct Node node;
	SHORT pad;
	char len;
	char str[0];
};

BPTR mkbstr(char *s) {
	int len = strlen(s);
	struct bstr_node *bs;

	bs = AllocMem(sizeof(*bs) + len, 0L);
	bs->len = len;
	memcpy(bs->str, s, len);
	AddHead(&bstrs, &bs->node);
	return MKBADDR(&bs->len);
}

struct cstr_node {
	struct Node node;
	int len;
	char str[0];
};

struct bstr {
	char len;
	char str[0];
};

char *mkcstr(BPTR s) {
	struct bstr *bs = BADDR(s);
	struct cstr_node *cs = AllocMem(sizeof(*cs) + bs->len + 1, 0L);
	cs->len = bs->len;
	memcpy(cs->str, bs->str, cs->len);
	cs->str[cs->len] = '\0';
	AddHead(&cstrs, &cs->node);
	return cs->str;
}

extern LONG dosbcpl(LONG *stack, LONG index, ...);

int main(void)
{
	struct SCSICmd scsicmd;
	int nactions;
	int oddaddress = 0, shortlength = 0;

	argsarray[ARG_DEVICE] = (LONG)device;
	argsarray[ARG_UNIT] = (LONG)&unit;

	if (DOSBase->dl_lib.lib_Version < 36) {
#define STACKFRAME_SIZE	(368L >> 2L)
#define RESULTARRAY_SIZE (656L >> 2L)
		LONG stack[STACKFRAME_SIZE];
		LONG result[RESULTARRAY_SIZE];
		LONG success = 0, i;

		success = dosbcpl(stack, 0x4E, mkbstr(ARGS_TEMPLATE),
						  MKBADDR(result), 80);

		if (success == DOSFALSE) {
			tprintf("Unable to read arguments\n");
			return RETURN_ERROR;
		}

		for (i = 0; i < ARG_NARG; i++) {
			if (result[i] == DOSFALSE) {
				continue;
			}

			switch (i) {
				static LONG unit;
				case ARG_DEVICE:
				case ARG_GET:
					argsarray[i] = (LONG)mkcstr(result[i]);
					break;
				case ARG_UNIT:
					/* FIXME: detect error */
					unit = atol(mkcstr(result[i]));
					argsarray[i] = (LONG)&unit;
					break;
				case ARG_LD:
				case ARG_LF:
				case ARG_LCD:
				case ARG_SCD:
				case ARG_CF:
				case ARG_ODD:
				case ARG_SHORT:
					argsarray[i] = result[i];
					break;
			}
		}
	} else {
		args = ReadArgs(ARGS_TEMPLATE, argsarray, NULL);
		if (args == NULL) {
			tprintf("Unable to read arguments\n");
			return RETURN_ERROR;
		}
	}

	datamem = AllocMem(4097UL, 0UL);

	nactions = (argsarray[ARG_LD] != 0) +
		   (argsarray[ARG_LF] != 0) +
		   (argsarray[ARG_LCD] != 0) +
		   (argsarray[ARG_SCD] != 0) +
		   (argsarray[ARG_GET] != 0) +
		   (argsarray[ARG_CF] != 0);
	if (nactions == 0) {
		tprintf("Must specify an action\n");
		return RETURN_ERROR;
	}
	if (nactions != 1) {
		tprintf("Must specify at most one action\n");
		return RETURN_ERROR;
	}

	if (!*(const char *)argsarray[ARG_DEVICE]) {
		tprintf("DEVICE must not be empty\n");
		return RETURN_ERROR;
	}

	if (argsarray[ARG_GET] &&
	    !*(const char *)argsarray[ARG_GET]) {
		tprintf("GET must not be empty\n");
		return RETURN_ERROR;
	}

	if (argsarray[ARG_SCD] &&
			!(UBYTE)*(LONG *)argsarray[ARG_SCD]) {
		tprintf("SETCD index must be greater than 0\n");
		return RETURN_ERROR;
	}

	device = (const char *)argsarray[ARG_DEVICE];
	unit = *(LONG *)argsarray[ARG_UNIT];
	oddaddress = argsarray[ARG_ODD] != 0L;
	shortlength = argsarray[ARG_SHORT] != 0L;

	msgport = CreatePort(NULL, 0L);
	if (msgport == NULL) {
		tprintf("Unable to create message port\n");
		return RETURN_ERROR;
	}
	ior = (struct IOStdReq *)CreateExtIO(msgport, sizeof(*ior));
	if (ior == NULL) {
		tprintf("Unable to create IO request\n");
		return RETURN_ERROR;
	}

	if(OpenDevice(device, unit, (struct IORequest *)ior, 0L)) {
		tprintf("Unable to open device %s unit %ld\n", device, unit);
		return RETURN_ERROR;
	}

	ior->io_Command = HD_SCSICMD;
	ior->io_Data = &scsicmd;
	ior->io_Length = sizeof(scsicmd);

	scsicmd.scsi_Command = command;
	scsicmd.scsi_CmdLength = sizeof(command);
	scsicmd.scsi_Data = (UWORD *)&datamem[oddaddress];
	scsicmd.scsi_Length = sizeof(data);
	scsicmd.scsi_Flags = SCSIF_READ;

	memset(&datamem[0], 0xff, 4097);

	if (argsarray[ARG_LD]) {
		if (shortlength) scsicmd.scsi_Length = 255UL;
		command[0] = 0xD9;
	} else if (argsarray[ARG_LF]) {
		command[0] = 0xD0;
	} else if (argsarray[ARG_LCD]) {
		command[0] = 0xD7;
	} else if (argsarray[ARG_SCD]) {
		command[0] = 0xD8;
		command[1] = (UBYTE)*(LONG *)argsarray[ARG_SCD] - 1;
	} else if (argsarray[ARG_GET]) {
		command[0] = 0xD0;
	} else if (argsarray[ARG_CF]) {
		if (shortlength) scsicmd.scsi_Length = 255UL;
		command[0] = 0xD2;
	}

	if (DoIO((struct IORequest *)ior)) {
		tprintf("Unable to send IO request: %ld\n", ior->io_Error);
		return RETURN_ERROR;
	}

	memcpy(data.data, &datamem[oddaddress], scsicmd.scsi_Actual);

	if (argsarray[ARG_LF]) {
		int i, nfiles;
		tprintf("%-32s %-10s\n", "Name", "Size");
		tprintf("-------------------------------------------\n");
		nfiles = scsicmd.scsi_Actual / sizeof(struct toolbox_file);
		for (i = 0; i < nfiles; i++) {
			struct toolbox_file *f = &data.files[i];
			tprintf("%-32s %-10ld\n", f->name, f->size);
		}
	} else if (argsarray[ARG_LCD]) {
		int i, nfiles;
		tprintf("%-6s %-32s %-10s\n", "Index", "Name", "Size");
		tprintf("--------------------------------------------------\n");
		nfiles = scsicmd.scsi_Actual / sizeof(struct toolbox_file);
		for (i = 0; i < nfiles; i++) {
			struct toolbox_file *f = &data.files[i];
			tprintf("%-6ld %-32s %-10ld\n",
			       f->index + 1, f->name, f->size);
		}
	} else if (argsarray[ARG_LD]) {
		int i;
		tprintf("%-3s %-32s\n", "ID", "Type");
		tprintf("------------------------------------\n");
		for (i = 0; i < scsicmd.scsi_Actual; i++) {
			UBYTE t = data.data[i];
			const char *s;
			if (t < sizeof(devicetypes)/sizeof(devicetypes[0])) {
				s = devicetypes[t];
			} else if (t == 255) {
				s = "Not enabled";
			} else {
				s = "Unknown";
			}
			tprintf("%-3ld %-32s\n", i, s);
		}
	} else if (argsarray[ARG_GET]) {
		ULONG i, nfiles;
		ULONG nblocks;
		ULONG bytesleft;
		const char *fpart = (const char *)argsarray[ARG_GET];
		nfiles = scsicmd.scsi_Actual / sizeof(struct toolbox_file);
		if (DOSBase->dl_lib.lib_Version >= 36) {
			fpart = FilePart(fpart);
		}
		for (i = 0; i < nfiles; i++) {
			struct toolbox_file *f = &data.files[i];
			if (!strcmp(fpart, f->name)) {
				break;
			}
		}
		if (i == nfiles) {
			tprintf("File \"%s\" not found\n", fpart);
			return RETURN_ERROR;
		}

		file = Open((const char *)argsarray[ARG_GET], MODE_NEWFILE);
		if (file == NULL) {
			tprintf("Unable to open file for writing\n");
			return RETURN_ERROR;
		}

		nblocks = (data.files[i].size / 4096) +
			  (data.files[i].size % 4096 ? 1 : 0);

		bytesleft = data.files[i].size;

		command[0] = 0xD1;
		command[1] = i;

		for (i = 0; i < nblocks; i++) {
			ULONG actual;
			memcpy(&command[2], &i, 4);
			if (shortlength) {
				if (i + 1 == nblocks) {
					scsicmd.scsi_Length = bytesleft;
				} else {
					scsicmd.scsi_Length = 4096;
				}
			}
			if (DoIO((struct IORequest *)ior)) {
				tprintf("Unable to send IO request: %ld\n",
				       ior->io_Error);
				return RETURN_ERROR;
			}

			memcpy(data.data, &datamem[oddaddress], scsicmd.scsi_Actual);

			tprintf("%s%s: Block %ld/%ld", i ? "\xd" : "",
			       (const char *)argsarray[ARG_GET],
			       i + 1, nblocks);

			actual = scsicmd.scsi_Actual;
			if (Write(file, data.data, actual) != actual) {
				tprintf("\nUnable to write to file\n");
				Close(file);
				file = NULL;
				DeleteFile((const char *)argsarray[ARG_GET]);
				return RETURN_ERROR;
			}
			bytesleft -= actual;
		}
		tprintf("\n");
	} else if (argsarray[ARG_CF]) {
		tprintf("%ld %ld\n", scsicmd.scsi_Actual, data.data[0]);
	}

	return RETURN_OK;
}

void _STD_cleanup(void)
{
	if (file)
		Close(file);

	if (ior) {
		if (ior->io_Device)
			CloseDevice((struct IORequest *)ior);
		DeleteExtIO((struct IORequest *)ior);
	}

	if (msgport)
		DeletePort(msgport);

	if (args)
		FreeArgs(args);

	while (!IsListEmpty(&bstrs)) {
		struct bstr_node *bs = (struct bstr_node *)RemHead(&bstrs);
		FreeMem(bs, sizeof(*bs) + bs->len);
	}

	while (!IsListEmpty(&cstrs)) {
		struct cstr_node *cs = (struct cstr_node *)RemHead(&cstrs);
		FreeMem(cs, sizeof(*cs) + cs->len + 1);
	}

	if (datamem)
		FreeMem(datamem, 4097UL);
}
