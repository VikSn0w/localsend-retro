/* Memory for the application. Retro68's template gives 1 MB; the core's cooperative threads (128 KB of stack each),
 * the 32 KB transfer buffers and Open Transport need more than that. */
#include "Processes.r"

resource 'SIZE' (-1) {
	reserved,
	acceptSuspendResumeEvents,
	reserved,
	canBackground,
	doesActivateOnFGSwitch,
	backgroundAndForeground,
	dontGetFrontClicks,
	ignoreChildDiedEvents,
	is32BitCompatible,
	isHighLevelEventAware,
	onlyLocalHLEvents,
	notStationeryAware,
	dontUseTextEditServices,
	reserved,
	reserved,
	reserved,
	4096 * 1024,
	3072 * 1024
};
