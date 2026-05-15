/* :ts=2 */
#include <devices/scsidisk.h>

#include <dos/dos.h>
#include <exec/exec.h>

#include <proto/alib.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <limits.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "toolbox_version.h"

UBYTE versiontag[] = VERSTAG;

static struct RDArgs *args;

#define ARGS_TEMPLATE	\
 "DEVICE,"						\
 "UNIT/N,"						\
 "LD=LISTDEVICES/S,"	\
 "LF=LISTFILES/S,"		\
 "LCD=LISTCDS/S,"			\
 "SCD=SETCD/N,"				\
 "GET,"								\
 "W=WIFI/S,"					\
 "SW=SCANWIFI/S"

enum {
	ARG_DEVICE,
	ARG_UNIT,
	ARG_LD,
	ARG_LF,
	ARG_LCD,
	ARG_SCD,
	ARG_GET,
	ARG_W,
	ARG_SW,
	ARG_NARG,
};
static LONG argsarray[ARG_NARG];

static const char *device = "scsi.device";
static LONG unit = 2;

static struct MsgPort *msgport;
static struct IOStdReq *ior;

struct toolbox_wifi_network {
	char  ssid[64];
	char  bssid[6];
	BYTE  rssi;
	UBYTE channel;
	UBYTE flags;
	UBYTE padding;
};

__aligned static union {
	struct toolbox_file {
		UBYTE index;
		UBYTE isdir;
		char  name[33];
		UBYTE sizemsb;
		ULONG size;
	} files[100];
	UBYTE devices[8];
	struct {
		USHORT size;
		struct toolbox_wifi_network info;
	} wifi_current;
	UBYTE wifi_scan_started;
	UBYTE wifi_scan_completed;
	struct {
		USHORT size;
		struct toolbox_wifi_network network[10];
	} wifi_scan;
	UBYTE data[4096];
} data;

__aligned static union {
	UBYTE cmd;
	struct {
		UBYTE cmd;
		UBYTE index;
		ULONG block;
		UBYTE pad[4];
	} get_file;
	struct {
		UBYTE cmd;
		UBYTE index;
		UBYTE pad[8];
	} set_next_cd;
	struct {
		UBYTE cmd;
		UBYTE subcmd;
		UBYTE pad0;
		UBYTE sizemsb;
		UBYTE sizelsb;
		UBYTE pad1[5];
	} wifi;
	UBYTE bytes[10];
} cdb;

#define TOOLBOX_LIST_FILES     0xD0
#define TOOLBOX_GET_FILE       0xD1
#define TOOLBOX_LIST_CDS       0xD7
#define TOOLBOX_SET_NEXT_CD    0xD8
#define TOOLBOX_LIST_DEVICES   0xD9
#define TOOLBOX_WIFI_CMD       0x1C

#define TOOLBOX_WIFI_CMD_SCAN         0x01
#define TOOLBOX_WIFI_CMD_COMPLETE     0x02
#define TOOLBOX_WIFI_CMD_SCAN_RESULTS 0x03
#define TOOLBOX_WIFI_CMD_INFO         0x04

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

static void __saveds __asm rawdofmt_putch(register __d0 char c,
																					register __a3 struct rawdofmt_data *d) {
	*d->b++ = c;
}

static void tprintf(STRPTR fmt, ...) {
	__aligned static char buffer[1025];
	struct rawdofmt_data d = {buffer};
	va_list ap;
	int len;
	va_start(ap, fmt);
	RawDoFmt(fmt, (APTR)ap, rawdofmt_putch, (APTR)&d);
	va_end(ap);
	len = d.b - buffer;
	if (len) {
		Write(Output(), buffer, len - 1);
	}
}

#define LIST_HEAD_INITIALIZER(l) {	\
	(struct Node *)&(l).lh_Tail,			\
	NULL,															\
	(struct Node *)&(l)								\
}

static struct List bstrs = LIST_HEAD_INITIALIZER(bstrs);
static struct List cstrs = LIST_HEAD_INITIALIZER(cstrs);

struct bstr {
	char len;
	char str[0];
};

struct bstr_node {
	struct Node node;
	SHORT pad;
	struct bstr bstr;
};

BPTR mkbstr(char *s) {
	int len = strlen(s);
	struct bstr_node *bs;

	bs = AllocMem(sizeof(*bs) + len, 0L);
	bs->bstr.len = len;
	memcpy(bs->bstr.str, s, len);
	AddHead(&bstrs, &bs->node);
	return MKBADDR(&bs->bstr);
}

struct cstr_node {
	struct Node node;
	int len;
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

	argsarray[ARG_DEVICE] = (LONG)device;
	argsarray[ARG_UNIT] = (LONG)&unit;

	if (DOSBase->dl_lib.lib_Version >= 36) {
		args = ReadArgs(ARGS_TEMPLATE, argsarray, NULL);

		if (args == NULL) {
			tprintf("Unable to read arguments: error %ld\n", IoErr());
			return RETURN_ERROR;
		}
	} else {
		static __aligned LONG stack[91];
		static __aligned LONG result[164];
		LONG success, i;

		success = dosbcpl(stack, 0x4E, mkbstr(ARGS_TEMPLATE),
											MKBADDR(result), 80);

		if (success == DOSFALSE) {
			tprintf("Unable to read arguments: error %ld\n", IoErr());
			return RETURN_ERROR;
		}

		for (i = 0; i < ARG_NARG; i++) {
			if (result[i] == DOSFALSE) {
				continue;
			}

			switch (i) {
				static LONG nresult[ARG_NARG];
				case ARG_DEVICE:
				case ARG_GET:
					argsarray[i] = (LONG)mkcstr(result[i]);
					break;
				case ARG_UNIT:
				case ARG_SCD:
					/* FIXME: detect error */
					nresult[i] = atol(mkcstr(result[i]));
					argsarray[i] = (LONG)&nresult[i];
					break;
				case ARG_LD:
				case ARG_LF:
				case ARG_LCD:
				case ARG_W:
				case ARG_SW:
					argsarray[i] = result[i];
					break;
			}
		}
	}

	nactions = (argsarray[ARG_LD] != 0) +
		   (argsarray[ARG_LF] != 0) +
		   (argsarray[ARG_LCD] != 0) +
		   (argsarray[ARG_SCD] != 0) +
		   (argsarray[ARG_GET] != 0) +
		   (argsarray[ARG_W] != 0)  +
		   (argsarray[ARG_SW] != 0) +
		   0;

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

	scsicmd.scsi_Command = (UBYTE *)&cdb;
	scsicmd.scsi_CmdLength = sizeof(cdb);
	scsicmd.scsi_Data = (UWORD *)&data;
	scsicmd.scsi_Flags = SCSIF_READ;

	if (argsarray[ARG_LD]) {
		scsicmd.scsi_Length = sizeof(data.devices);
		cdb.cmd = TOOLBOX_LIST_DEVICES;
	} else if (argsarray[ARG_LF]) {
		scsicmd.scsi_Length = sizeof(data.files);
		cdb.cmd = TOOLBOX_LIST_FILES;
	} else if (argsarray[ARG_LCD]) {
		scsicmd.scsi_Length = sizeof(data.files);
		cdb.cmd = TOOLBOX_LIST_CDS;
	} else if (argsarray[ARG_SCD]) {
		cdb.cmd = TOOLBOX_SET_NEXT_CD;
		cdb.set_next_cd.index = (UBYTE)*(LONG *)argsarray[ARG_SCD] - 1;
	} else if (argsarray[ARG_GET]) {
		scsicmd.scsi_Length = sizeof(data.files);
		cdb.cmd = TOOLBOX_LIST_FILES;
	} else if (argsarray[ARG_W]) {
		scsicmd.scsi_Length = sizeof(data.wifi_current);
		cdb.cmd = TOOLBOX_WIFI_CMD;
		cdb.wifi.subcmd = TOOLBOX_WIFI_CMD_INFO;
		cdb.wifi.sizemsb = sizeof(data.wifi_current) >> 8;
		cdb.wifi.sizelsb = sizeof(data.wifi_current);
	} else if (argsarray[ARG_SW]) {
		scsicmd.scsi_Length = sizeof(data.wifi_scan_started);
		cdb.cmd = TOOLBOX_WIFI_CMD;
		cdb.wifi.subcmd = TOOLBOX_WIFI_CMD_SCAN;
	}


	if (DoIO((struct IORequest *)ior)) {
		tprintf("Unable to send IO request: %ld\n", ior->io_Error);
		return RETURN_ERROR;
	}

	if (argsarray[ARG_LF]) {
		ULONG nfiles = scsicmd.scsi_Actual / sizeof(struct toolbox_file);
		ULONG i;

		tprintf("%-32s %-11s\n", "Name", "Size");
		tprintf("--------------------------------------------\n");

		for (i = 0; i < nfiles; i++) {
			struct toolbox_file *f = &data.files[i];

			tprintf("%-32s %-10lu%s\n", f->name,
							f->sizemsb ? ULONG_MAX : f->size,
							f->sizemsb ? "+" : "");
		}
	} else if (argsarray[ARG_LCD]) {
		ULONG nfiles = scsicmd.scsi_Actual / sizeof(struct toolbox_file);
		ULONG i;

		tprintf("%-6s %-32s %-11s\n", "Index", "Name", "Size");
		tprintf("---------------------------------------------------\n");

		for (i = 0; i < nfiles; i++) {
			struct toolbox_file *f = &data.files[i];

			tprintf("%-6ld %-32s %-10lu%s\n",
							f->index + 1, f->name,
							f->sizemsb ? ULONG_MAX : f->size,
							f->sizemsb ? "+" : "");
		}
	} else if (argsarray[ARG_LD]) {
		ULONG i;

		tprintf("%-3s %-32s\n", "ID", "Type");
		tprintf("------------------------------------\n");

		for (i = 0; i < scsicmd.scsi_Actual; i++) {
			UBYTE t = data.devices[i];
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
		ULONG nfiles = scsicmd.scsi_Actual / sizeof(struct toolbox_file);
		const char *fpart = (const char *)argsarray[ARG_GET];
		ULONG i;
		ULONG nblocks;

		if (DOSBase->dl_lib.lib_Version >= 36) {
			fpart = FilePart(fpart);
		} else {
			const char *lslash = strrchr(fpart, '/');
			if (lslash) {
				fpart = lslash + 1;
			}
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

		if (data.files[i].sizemsb ||
				data.files[i].size > INT_MAX) {
			tprintf("File too large (>%ld bytes)\n", INT_MAX);
			return RETURN_ERROR;
		}

		file = Open((const char *)argsarray[ARG_GET], MODE_NEWFILE);

		if (file == NULL) {
			tprintf("Unable to open file for writing: error %ld\n", IoErr());
			return RETURN_ERROR;
		}

		nblocks = (data.files[i].size / 4096) +
			  (data.files[i].size % 4096 ? 1 : 0);

		scsicmd.scsi_Length = sizeof(data.data);
		cdb.cmd = TOOLBOX_GET_FILE;
		cdb.get_file.index = i;

		tprintf("%s: Block %ld/%ld",
						(const char *)argsarray[ARG_GET],
						0, nblocks);

		for (i = 0; i < nblocks; i++) {
			ULONG actual;

			cdb.get_file.block = i;

			if (DoIO((struct IORequest *)ior)) {
				tprintf("Unable to send IO request: %ld\n",
								ior->io_Error);
				return RETURN_ERROR;
			}

			tprintf("\xd%s: Block %ld/%ld",
							(const char *)argsarray[ARG_GET],
							i + 1, nblocks);

			actual = scsicmd.scsi_Actual;

			if (Write(file, data.data, actual) != actual) {
				tprintf("\nUnable to write to file: error %ld\n", IoErr());
				Close(file);
				file = NULL;
				DeleteFile((const char *)argsarray[ARG_GET]);
				return RETURN_ERROR;
			}
		}
		tprintf("\n");
	} else if (argsarray[ARG_W]) {
		struct toolbox_wifi_network *w = &data.wifi_current.info;

		if (data.wifi_current.size != sizeof(*w)) {
			tprintf("Unknown size (%lu) returned\n", data.wifi_current.size);
			return RETURN_ERROR;
		}

		if (w->ssid[0]) {
			tprintf("SSID   : %-64s\n", w->ssid);
			tprintf("RSSI   : %ld\n", w->rssi);
			tprintf("Channel: %lu\n", w->channel);
			tprintf("Flags  : 0x%lx\n", w->flags);
		} else {
			tprintf("WiFi not connected\n");
		}
	} else if (argsarray[ARG_SW]) {
		int i, rounds = 0;

		if (!data.wifi_scan_started) {
			tprintf("Unable to start WiFi scan (%ld)\n", data.wifi_scan_started);
			return RETURN_ERROR;
		}

		scsicmd.scsi_Length = sizeof(data.wifi_scan_completed);
		cdb.wifi.subcmd = TOOLBOX_WIFI_CMD_COMPLETE;

		do {
				if (rounds == 125) {
					tprintf("Timeout while waiting for WiFi scan to complete\n");
					return RETURN_ERROR;
				}
				if (rounds) {
					Delay(10);
				}
				if (DoIO((struct IORequest *)ior)) {
					tprintf("Unable to send IO request: %ld\n", ior->io_Error);
					return RETURN_ERROR;
				}
				rounds++;
		} while (!data.wifi_scan_completed);

		scsicmd.scsi_Length = sizeof(data.wifi_scan);
		cdb.wifi.subcmd = TOOLBOX_WIFI_CMD_SCAN_RESULTS;
		cdb.wifi.sizemsb = (sizeof(data.wifi_scan) & 0xff00) >> 8;
		cdb.wifi.sizelsb = (sizeof(data.wifi_scan) & 0x00ff);

		if (DoIO((struct IORequest *)ior)) {
			tprintf("Unable to send IO request: %ld\n", ior->io_Error);
			return RETURN_ERROR;
		}

		tprintf("%-32s %-4s %-7s\n", "SSID", "RSSI", "Channel");
		tprintf("---------------------------------------------\n");

		for (i = 0; i < 10; i++) {
			struct toolbox_wifi_network *w = &data.wifi_scan.network[i];

			if (w->ssid[0] == 0) {
				break;
			}

			if (strlen(w->ssid) > 32) {
				memcpy(&w->ssid[29], "...\0", 4);
			}

			tprintf("%-32s %-4ld %-7ld\n", w->ssid, w->rssi, w->channel);
		}
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
		FreeMem(bs, sizeof(*bs) + bs->bstr.len);
	}

	while (!IsListEmpty(&cstrs)) {
		struct cstr_node *cs = (struct cstr_node *)RemHead(&cstrs);
		FreeMem(cs, sizeof(*cs) + cs->len + 1);
	}
}
