/* :ts=2 */
#include <devices/scsidisk.h>

#include <exec/exec.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "toolbox_version.h"

static UBYTE versiontag[] = VERSTAG;

static struct RDArgs *args;

enum {
	ARG_DEVICE,
	ARG_UNIT,
	ARG_LD,
	ARG_LF,
	ARG_LCD,
	ARG_SCD,
	ARG_GET,
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

int main(void)
{
	struct SCSICmd scsicmd;
	int nactions;

	if (DOSBase->dl_lib.lib_Version < 36) {
		tprintf("dos.library v36 or later is required\n");
		return RETURN_ERROR;
	}

	argsarray[ARG_DEVICE] = (LONG)device;
	argsarray[ARG_UNIT] = (LONG)&unit;

	args = ReadArgs("DEVICE,UNIT/N,LD=LISTDEVICES/S,LF=LISTFILES/S,"
			"LCD=LISTCDS/S,SCD=SETCD/N,GET",
			argsarray, NULL);
	if (args == NULL) {
		PrintFault(IoErr(), "Unable to read arguments");
		return RETURN_ERROR;
	}

	nactions = (argsarray[ARG_LD] != 0) +
		   (argsarray[ARG_LF] != 0) +
		   (argsarray[ARG_LCD] != 0) +
		   (argsarray[ARG_SCD] != 0) +
		   (argsarray[ARG_GET] != 0);
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

	msgport = CreateMsgPort();
	if (msgport == NULL) {
		tprintf("Unable to create message port\n");
		return RETURN_ERROR;
	}
	ior = (struct IOStdReq *)CreateIORequest(msgport, sizeof(*ior));
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
	if (argsarray[ARG_LD]) {
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
	}
	scsicmd.scsi_Command = command;
	scsicmd.scsi_CmdLength = sizeof(command);
	scsicmd.scsi_Data = (UWORD *)&data;
	scsicmd.scsi_Length = sizeof(data);
	scsicmd.scsi_Flags = SCSIF_READ;

	if (DoIO((struct IORequest *)ior)) {
		tprintf("Unable to send IO request: %ld\n", ior->io_Error);
		return RETURN_ERROR;
	}

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
		const char *fpart;
		nfiles = scsicmd.scsi_Actual / sizeof(struct toolbox_file);
		fpart = FilePart((const char *)argsarray[ARG_GET]);
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
			PrintFault(IoErr(),
				   "Unable to open file for writing");
			return RETURN_ERROR;
		}

		nblocks = (data.files[i].size / 4096) +
			  (data.files[i].size % 4096 ? 1 : 0);

		command[0] = 0xD1;
		command[1] = i;

		for (i = 0; i < nblocks; i++) {
			ULONG actual;
			memcpy(&command[2], &i, 4);
			if (DoIO((struct IORequest *)ior)) {
				tprintf("Unable to send IO request: %ld\n",
				       ior->io_Error);
				return RETURN_ERROR;
			}

			tprintf("%s%s: Block %ld/%ld", i ? "\xd" : "",
			       (const char *)argsarray[ARG_GET],
			       i + 1, nblocks);

			actual = scsicmd.scsi_Actual;
			if (Write(file, data.data, actual) != actual) {
				PrintFault(IoErr(),
					   "\nUnable to write to file");
				Close(file);
				file = NULL;
				DeleteFile((const char *)argsarray[ARG_GET]);
				return RETURN_ERROR;
			}
		}
		tprintf("\n");
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
		DeleteIORequest(ior);
	}

	if (msgport)
		DeleteMsgPort(msgport);

	if (args)
		FreeArgs(args);
}
