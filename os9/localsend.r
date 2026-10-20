/* Resources for LocalSend for Mac OS 9. Compile with Rez (MPW) after linking. */

#include "Types.r"
#include "SysTypes.r"
#include "Menus.r"
#include "Dialogs.r"
#include "Processes.r"

/* the OS 9 icon family (ICN#, icl8, ics#, ics8, id 128) */
#include "localsend_icons.r"

resource 'MBAR' (128) {
	{ 128, 129, 130 }
};

resource 'MENU' (128, preload) {
	128, textMenuProc, 0x7FFFFFFF, enabled, apple,
	{
		"About LocalSend...", noicon, nokey, nomark, plain;
		"-", noicon, nokey, nomark, plain
	}
};

resource 'MENU' (129, preload) {
	129, textMenuProc, 0x7FFFFFFF, enabled, "File",
	{
		"Send File...", noicon, "O", nomark, plain;
		"Refresh", noicon, "R", nomark, plain;
		"-", noicon, nokey, nomark, plain;
		"Quit", noicon, "Q", nomark, plain
	}
};

resource 'MENU' (130, preload) {
	130, textMenuProc, 0x7FFFFFFF, enabled, "Edit",
	{
		"Undo", noicon, "Z", nomark, plain;
		"-", noicon, nokey, nomark, plain;
		"Cut", noicon, "X", nomark, plain;
		"Copy", noicon, "C", nomark, plain;
		"Paste", noicon, "V", nomark, plain;
		"Clear", noicon, nokey, nomark, plain
	}
};

/* About box */
resource 'ALRT' (128) {
	{60, 60, 190, 400},
	128,
	{
		OK, visible, silent,
		OK, visible, silent,
		OK, visible, silent,
		OK, visible, silent
	},
	alertPositionMainScreen
};

resource 'DITL' (128) {
	{
		{98, 258, 118, 328}, Button { enabled, "OK" };
		{12, 12, 90, 328}, StaticText { disabled,
			"LocalSend for Mac OS 9, version 0.1. Compatible with LocalSend (localsend.org). "
			"It receives files over plain HTTP and sends to devices that have encryption turned off." }
	}
};

/* "x wants to send you y" */
resource 'ALRT' (129) {
	{60, 60, 190, 400},
	129,
	{
		OK, visible, silent,
		OK, visible, silent,
		OK, visible, silent,
		OK, visible, silent
	},
	alertPositionMainScreen
};

resource 'DITL' (129) {
	{
		{98, 258, 118, 328}, Button { enabled, "Accept" };
		{98, 168, 118, 248}, Button { enabled, "Decline" };
		{12, 12, 88, 328}, StaticText { disabled, "^0 wants to send you ^1. Accept?" }
	}
};

/* Finder signature */
type 'LSPC' as 'STR ';
resource 'LSPC' (0) {
	"LocalSend for Mac OS 9 0.1"
};

resource 'FREF' (128) {
	'APPL', 0, ""
};

resource 'BNDL' (128) {
	'LSPC', 0,
	{
		'ICN#', { 0, 128 };
		'FREF', { 0, 128 }
	}
};

resource 'vers' (1) {
	0x00, 0x10, release, 0x00, verUS,
	"0.1",
	"0.1, LocalSend for Mac OS 9"
};

/* Memory: the core allocates a few 32 KB transfer buffers and Open Transport needs room */
resource 'SIZE' (-1) {
	reserved,
	acceptSuspendResumeEvents,
	reserved,
	canBackground,
	doesActivateOnFGSwitch,
	backgroundAndForeground,
	dontGetFrontClicks,
	ignoreAppDiedEvents,
	is32BitCompatible,
	notHighLevelEventAware,
	onlyLocalHLEvents,
	notStationeryAware,
	dontUseTextEditServices,
	reserved,
	reserved,
	reserved,
	5120 * 1024,
	3072 * 1024
};
