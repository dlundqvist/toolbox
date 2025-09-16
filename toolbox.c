#include <devices/scsidisk.h>

#include <exec/exec.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include <stdio.h>
#include <string.h>

#include "toolbox_rev.h"

static UBYTE versiontag[] = VERSTAG;

static struct RDArgs *args;

enum {
	ARG_DEVICE,
	ARG_UNIT,
	ARG_LD,
	ARG_LF,
	ARG_LCD,
	ARG_SCD,
	ARG_NARG,
};
static LONG argsarray[ARG_NARG];

static const char *device = "scsi.device";
static LONG unit = 2;

static struct MsgPort *msgport;
static struct IOStdReq *ior;

union toolbox {
	struct toolbox_file {
		UBYTE index;
		UBYTE isdir;
		char  name[33];
		ULONG size;
	} files[100];
};

static UBYTE command[10];
static __aligned UBYTE data[sizeof(union toolbox)];

static union toolbox *toolbox = (union toolbox *)data;

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


int main(void)
{
	struct SCSICmd scsicmd;
	int nactions;

	argsarray[ARG_DEVICE] = (LONG)device;
	argsarray[ARG_UNIT] = (LONG)&unit;

	args = ReadArgs("DEVICE,UNIT/N,LD=LISTDEVICES/S,LF=LISTFILES/S,"
			"LCD=LISTCDS/S,SCD=SETCD/N",
			argsarray, NULL);
	if (args == NULL) {
		PrintFault(IoErr(), "Unable to read arguments");
		return 1;
	}

	nactions = (argsarray[ARG_LD] != 0) +
		   (argsarray[ARG_LF] != 0) +
		   (argsarray[ARG_LCD] != 0) +
		   (argsarray[ARG_SCD] != 0);
	if (nactions == 0) {
		Printf("Must specify an action\n");
		return 1;
	}
	if (nactions != 1) {
		Printf("Must only specify one action\n");
		return 1;
	}

	device = (const char *)argsarray[ARG_DEVICE];
	unit = *(LONG *)argsarray[ARG_UNIT];

	msgport = CreateMsgPort();
	if (msgport == NULL) {
		Printf("Unable to create message port\n");
		return 1;
	}
	ior = (struct IOStdReq *)CreateIORequest(msgport, sizeof(*ior));
	if (ior == NULL) {
		Printf("Unable to create IO request\n");
		return 1;
	}

	if(OpenDevice(device, unit, (struct IORequest *)ior, 0L)) {
		Printf("Unable to open device %s unit %ld\n", device, unit);
		return 1;
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
		command[1] = (UBYTE)*(LONG *)argsarray[ARG_SCD];
	}
	scsicmd.scsi_Command = command;
	scsicmd.scsi_CmdLength = sizeof(command);
	scsicmd.scsi_Data = (UWORD *)data;
	scsicmd.scsi_Length = sizeof(data);
	scsicmd.scsi_Flags = SCSIF_READ;

	if (DoIO((struct IORequest *)ior)) {
		Printf("Unable to send IO request: %ld\n", ior->io_Error);
		return 1;
	}

	if (argsarray[ARG_LF] || argsarray[ARG_LCD]) {
		int i, nfiles;
		Printf("%-6s %-32s %-10s\n", "Index", "Name", "Size");
		Printf("--------------------------------------------------\n");
		nfiles = scsicmd.scsi_Actual / sizeof(struct toolbox_file);
		for (i = 0; i < nfiles; i++) {
			struct toolbox_file *f = &toolbox->files[i];
			Printf("%-6ld %-32s %-10ld\n",
			       f->index, f->name, f->size);
		}
	} else if (argsarray[ARG_LD]) {
		int i;
		Printf("%-3s %-32s\n", "ID", "Type");
		Printf("------------------------------------\n");
		for (i = 0; i < scsicmd.scsi_Actual; i++) {
			UBYTE t = scsicmd.scsi_Data[i];
			const char *s;
			if (t < sizeof(devicetypes)/sizeof(devicetypes[0])) {
				s = devicetypes[t];
			} else if (t == 255) {
				s = "Not enabled";
			} else {
				s = "Unknown type";
			}
			Printf("%-2ld %s (%ld)\n", i, s, t);
		}
	}

	return 0;
}

void _STD_cleanup(void)
{
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
