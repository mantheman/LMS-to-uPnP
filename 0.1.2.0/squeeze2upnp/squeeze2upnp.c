/*
 *  Squeeze2upnp - LMS to uPNP gateway
 *
 *  Squeezelite : (c) Adrian Smith 2012-2014, triode1@btinternet.com
 *  Additions & gateway : (c) Philippe 2014, philippe_44@outlook.com
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
 *
 */

#include <math.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "squeezedefs.h"
#include "squeeze2upnp.h"
#include "upnpdebug.h"
#include "upnptools.h"
#include "webserver.h"
#include "util_common.h"
#include "util.h"
#include "avt_util.h"
#include "mr_util.h"

/*
TODO :
- for no pause, the solution will be to send the elapsed time to LMS through CLI so that it does take care of the seek
- samplerate management will have to be reviewed when decode will be used
*/

/*----------------------------------------------------------------------------*/
/* globals initialized */
/*----------------------------------------------------------------------------*/
char				glBaseVDIR[] = "LMS2UPNP";
char				glSQServer[SQ_STR_LENGTH] = "?";
u8_t				glMac[6] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05 };
sq_log_level_t		glLog = { lINFO, lINFO, lINFO, lINFO, lINFO, lINFO, lINFO, lINFO, lINFO};
#if LINUX || FREEBSD
bool				glDaemonize = false;
#endif
char				*glLogFile;

tMRConfig			glMRConfig = {
							-3L,
							SQ_STREAM,
							false,
							false,
							true,
							0,
							true,
							"",
							false,
							false,
							true,
							"0:0, 400:10, 700:20, 1200:30, 2050:40, 3800:50, 6600:60, 12000:70, 21000:80, 37000:90, 65535:100",
					};

sq_dev_param_t glDeviceParam = {
					 // both are multiple of 3*4(2) for buffer alignement on sample
					(200 * 1024 * (4*3)),
					(200 * 1024 * (4*3)),
					SQ_STREAM,
					{ 	SQ_RATE_384000, SQ_RATE_352000, SQ_RATE_192000, SQ_RATE_176400,
						SQ_RATE_96000, SQ_RATE_48000, SQ_RATE_44100,
						SQ_RATE_32000, SQ_RATE_24000, SQ_RATE_22500, SQ_RATE_16000,
					SQ_RATE_12000, SQ_RATE_11025, SQ_RATE_8000, 0 },
					true,
					-1,
					100,
					"mp3",
					SQ_RATE_48000,
					L24_PACKED_LPCM,
					".",
					-1L,
				} ;

/*----------------------------------------------------------------------------*/
/* globals */
/*----------------------------------------------------------------------------*/
ithread_t 			glTimerThread;
bool		 		glTimerOn = true;
unsigned int 		glPort;
char 				glIPaddress[128] = "";
ithread_mutex_t 	glDeviceListMutex;
UpnpClient_Handle 	glControlPointHandle;
struct sMR 		  	*glDeviceList = NULL;
struct sMR		  	*glSQ2MRList = NULL;
void				*glConfigID = NULL;
char				glConfigName[SQ_STR_LENGTH] = "./config.xml";

/*----------------------------------------------------------------------------*/
/* consts or pseudo-const*/
/*----------------------------------------------------------------------------*/
static const char 	MEDIA_RENDERER[] 	= "urn:schemas-upnp-org:device:MediaRenderer:1";

static const char 	cMRPlayer[] 		= "";
static const char 	cLogitech[] 		= "Logitech";
static const struct cSearchedSRV_s
{
 char 	name[RESOURCE_LENGTH];
 int	idx;
} cSearchedSRV[] = {	{AV_TRANSPORT, AVT_SRV_IDX},
						{RENDERING_CTRL, REND_SRV_IDX},
						{CONNECTION_MGR, CNX_MGR_IDX}
				   };

const int	NB_SRV = sizeof(cSearchedSRV) / sizeof(struct cSearchedSRV_s);

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static log_level 		loglevel = lWARN;
static char usage[];
static char license[];

/*----------------------------------------------------------------------------*/
/* prototypes */
/*----------------------------------------------------------------------------*/
static void *TimerLoop(void *args);
static void AddMRDevice(IXML_Document *DescDoc, const char *location, int expires);

/*----------------------------------------------------------------------------*/
bool sq_callback(sq_dev_handle_t handle, void *caller, sq_action_t action, int cookie, void *param)
{
	struct sMR *device = caller;
	char *p = (char*) param;
	bool rc = true;

	if (!device)	{
		LOG_ERROR("No caller ID in callback", NULL);
		return false;
	}

	if (action == SQ_ONOFF) {
		device->on = *((bool*) param);
		LOG_INFO("[%p]: device set on/off %d", caller, device->on);
	}

	if (!device->on) {
		LOG_INFO("[%p]: device off or not controlled by LMS", caller);
		return false;
	}

	LOG_DEBUG("callback for %s", device->FriendlyName);
	ithread_mutex_lock(&device->Mutex);

	switch (action) {

		case SQ_SETFORMAT: {
			sq_seturi_t *p = (sq_seturi_t*) param;

			LOG_INFO("[%p]: codec:%c, ch:%d, s:%d, r:%d", device, p->content_type[0],
										p->channels, p->sample_size, p->sample_rate);
			if (!SetContentType(device->ProtocolCap, param)) {
				LOG_ERROR("[%p]: no matching codec in player (%s)", caller, p->proto_info);
				rc = false;
			}
			break;
		}
		case SQ_SETNEXTURI: {
			char uri[RESOURCE_LENGTH];
			sq_seturi_t *p = (sq_seturi_t*) param;

			// if port and/or ip are 0 or "", means that the CP@ shall be used
			if (p->port)
				sprintf(uri, "http://%s:%d/%s", p->ip, p->port, p->urn);
			else
				sprintf(uri, "http://%s:%d/%s/%s", glIPaddress, glPort, glBaseVDIR, p->urn);

			NFREE(device->NextURI);

			strcpy(device->NextProtInfo, p->proto_info);
			if (device->Config.AcceptNextURI)
				AVTSetNextURI(device->Service[AVT_SRV_IDX].ControlURL, uri, p->proto_info, (void*) device->seqN++);

			// to know what is expected next
			device->NextURI = (char*) malloc(strlen(uri) + 1);
			strcpy(device->NextURI, uri);
			LOG_INFO("[%p]: next URI set %s", device, device->NextURI);
			break;
		}
		case SQ_SETURI:	{
			char uri[RESOURCE_LENGTH];
			sq_seturi_t *p = (sq_seturi_t*) param;

			// if port and/or ip are 0 or "", means that the CP@ shall be used
			if (p->port)
				sprintf(uri, "http://%s:%d/%s", p->ip, p->port, p->urn);
			else
				sprintf(uri, "http://%s:%d/%s/%s", glIPaddress, glPort, glBaseVDIR, p->urn);

			// to detect properly transition
			NFREE(device->CurrentURI);
			// check side effect of this
			NFREE(device->NextURI);
			// end check

			AVTSetURI(device->Service[AVT_SRV_IDX].ControlURL, uri, p->proto_info, (void*) device->seqN++);
			device->CurrentURI = (char*) malloc(strlen(uri) + 1);
			strcpy(device->CurrentURI, uri);
			LOG_INFO("[%p]: current URI set %s", device, device->CurrentURI);
			break;
		}
		case SQ_UNPAUSE:
			if (device->PausedTime && device->Config.SeekAfterPause) {
				sq_set_time(device->SqueezeHandle, device->PausedTime);
			}
		case SQ_PLAY:
			if (device->CurrentURI) {
				device->PausedTime = 0;
				QueueAction(handle, caller, action, cookie, param, false);
				device->sqState = SQ_PLAY;
				if (device->Config.VolumeOnPlay)
					SetVolume(device->Service[REND_SRV_IDX].ControlURL, device->Volume, (void*) device->seqN++);
			}
			else rc = false;
			break;
		case SQ_STOP:
			AVTBasic(device->Service[AVT_SRV_IDX].ControlURL, "Stop", (void*) device->seqN++);
			FlushActionList(device);
			device->sqState = action;
			break;
		case SQ_PAUSE:
			if (device->Config.SeekAfterPause)
				device->PausedTime = sq_get_time(device->SqueezeHandle);
			QueueAction(handle, caller, action, cookie, param, false);
			device->sqState = action;
			break;
		case SQ_SEEK:
//			AVTSeek(device->Service[AVT_SRV_IDX].ControlURL, *(u16_t*) p);
			break;
		case SQ_VOLUME: {
			u32_t Volume = *(u32_t*)p;
			int i = 0;
			double a2, b2, a1 = 0, b1 = 0;

			for (i = 0; i < 32 && Volume > device->VolumeCurve[i].a; i++);

			a1 = (i) ? device->VolumeCurve[i-1].a : 0;
			b1 = (i) ? device->VolumeCurve[i-1].b : 0;
			a2 = device->VolumeCurve[i].a;
			b2 = device->VolumeCurve[i].b;
			device->Volume = Volume * (b1-b2)/(a1-a2) + b1 - a1*(b1-b2)/(a1-a2);

			if (!device->Config.VolumeOnPlay || device->sqState == SQ_PLAY)
				SetVolume(device->Service[REND_SRV_IDX].ControlURL, device->Volume, (void*) device->seqN++);
			break;
		}
		default:
			break;
	}

	ithread_mutex_unlock(&device->Mutex);
	return rc;
}


/*----------------------------------------------------------------------------*/
void SyncNotifState(char *State, struct sMR* Device)
{
	struct sAction *Action = NULL;

	ithread_mutex_lock(&Device->Mutex);

	// in transitioning mode, do nothing, just wait
	if (!strcmp(State, "TRANSITIONING")) {
		if (Device->State != TRANSITIONING) {
			LOG_INFO("%s: uPNP transition", Device->FriendlyName);
		}
		Device->State = TRANSITIONING;
		ithread_mutex_unlock(&Device->Mutex);
		return;
	}

	Action = UnQueueAction(Device, true);

	if (!strcmp(State, "STOPPED")) {
		if (Device->State != STOPPED) {
			LOG_INFO("%s: uPNP stop", Device->FriendlyName);
			if (!Device->Config.AcceptNextURI && Device->NextURI) {
				int WaitFor = Device->seqN++;

				// fake a "SETURI" and a "PLAY" request
				NFREE(Device->CurrentURI);
				Device->CurrentURI = malloc(strlen(Device->NextURI) + 1);
				strcpy(Device->CurrentURI, Device->NextURI);
				NFREE(Device->NextURI);

				AVTSetURI(Device->Service[AVT_SRV_IDX].ControlURL, Device->CurrentURI, Device->NextProtInfo, (void*) WaitFor);

				/*
				Need to queue to wait for the SetURI to be accepted, otherwise
				the current URI will be played, creating a "blurb" effect
				*/
				QueueAction(Device->SqueezeHandle, Device, SQ_PLAY, WaitFor, NULL, true);

				// fake the change
				sq_notify(Device->SqueezeHandle, Device, SQ_TRACK_CHANGE, 0, NULL);

				LOG_INFO("[%p]: no gapless %s", Device, Device->CurrentURI);
			}
			else sq_notify(Device->SqueezeHandle, Device, SQ_STOP, 0, NULL);
			Device->State = STOPPED;
		 }
	}

	if (!strcmp(State, "PLAYING")) {
		if (Device->State != PLAYING) {
			LOG_INFO("%s: uPNP playing", Device->FriendlyName);
			sq_notify(Device->SqueezeHandle, Device, SQ_PLAY, 0, NULL);

			if (Device->Config.ForceVolume && Device->Config.ProcessMode != SQ_LMSUPNP)
				SetVolume(Device->Service[REND_SRV_IDX].ControlURL, Device->Volume, (void*) Device->seqN++);

			if (Action && Action->Action == SQ_PLAY) {
				UnQueueAction(Device, false);
				NFREE(Action);
			}
			Device->State = PLAYING;
		}
	}

	if (!strcmp(State, "PAUSED_PLAYBACK")) {
		if (Device->State != PAUSED) {
			LOG_INFO("%s: uPNP pause", Device->FriendlyName);
			if (Action && Action->Action == SQ_PAUSE) {
				UnQueueAction(Device, false);
				NFREE(Action);
			}
			Device->State = PAUSED;
		}
	}

	if (Action && (!Action->Ordered || Action->Cookie <= Device->LastAckAction)) {
		struct sAction *p = UnQueueAction(Device, false);

		if (p != Action) {
			LOG_ERROR("[%p]: mutex issue %d, %d", p->Cookie, Action->Cookie);
		}

		switch (Action->Action) {
		case SQ_UNPAUSE:
		case SQ_PLAY:
			AVTPlay(Action->Caller->Service[AVT_SRV_IDX].ControlURL, (void*) Device->seqN++);
			break;
		case SQ_PAUSE:
			AVTBasic(Action->Caller->Service[AVT_SRV_IDX].ControlURL, "Pause", (void*) Device->seqN++);
			break;
		default:
			break;
		}
		NFREE(Action);
	}

	ithread_mutex_unlock(&Device->Mutex);
}

#ifdef SUBSCRIBE_EVENT
/*----------------------------------------------------------------------------*/
void HandleStateEvent(struct Upnp_Event *Event, void *Cookie)
{
	struct sMR *Device;
	IXML_Document *StateDoc, *VarDoc = Event->ChangedVariables;
	char  *State = NULL;
	char  *LastChange = NULL;
	char  *CurrentURI;

	Device = Sid2Device(Event->Sid);
	if (!Device) {
		LOG_SDEBUG("no Squeeze device (yet) for %s", Event->Sid);
		return;
	}

	if (Device->Magic != MAGIC) {
		LOG_ERROR("[%p]: Wrong magic ", Device);
		return;
	}

	if (!Device->on) {
		LOG_INFO("[%p]: device off, ignored ", Device);
		return;
	}

	LastChange = XMLGetFirstDocumentItem(VarDoc, "LastChange");
	LOG_SDEBUG("Data event %s %u %s", Event->Sid, Event->EventKey, LastChange);
	if (LastChange) free(LastChange);

	State = XMLGetChangeItem(VarDoc, "TransportState");
	if (State){
		SyncNotifState(State, Device);
		free(State);
	}

	CurrentURI = XMLGetChangeItem(VarDoc, "CurrentTrackURI");
	if (CurrentURI) {
		SyncNotifURI(CurrentURI, Device);
		free(CurrentURI);
	 }

	UnQueueAction(Device, false);
}
#endif


/*----------------------------------------------------------------------------*/
int CallbackActionHandler(Upnp_EventType EventType, void *Event, void *Cookie)
{
	LOG_SDEBUG("Action Handler : %i (cookie %d)", EventType, Cookie);

	switch ( EventType ) {
		case UPNP_CONTROL_ACTION_COMPLETE: 	{
			struct Upnp_Action_Complete *Action = (struct Upnp_Action_Complete *)Event;
			struct sMR *p;
			char   *r;

			p = CURL2Device(Action->CtrlUrl);
			if (!p) break;

			p->LastAckAction = (int) Cookie;
			LOG_SDEBUG("[%p]: ac %i %s (cookie %d)", p, EventType, Action->CtrlUrl, Cookie);

			// time position response
			r = XMLGetFirstDocumentItem(Action->ActionResult, "RelTime");
			if (r) {
				p->Elapsed = Time2Int(r);
				LOG_SDEBUG("[%p]: position %d (cookie %d)", p, p->Elapsed, Cookie);
				// discard any time info unless we are confirmed playing
				if (p->State == PLAYING)
					sq_notify(p->SqueezeHandle, p, SQ_TIME, 0, &p->Elapsed);
			}
			NFREE(r);

			// transport state response
			r = XMLGetFirstDocumentItem(Action->ActionResult, "CurrentTransportState");
			if (r) SyncNotifState(r, p);
			NFREE(r);

			// URI detection response
			r = XMLGetFirstDocumentItem(Action->ActionResult, "CurrentURI");
			if (r && p->CurrentURI) {
				// mutex has to be set BEFORE test and unset BEFORE notification
				ithread_mutex_lock(&p->Mutex);
				/*
				an URI change detection is only valid if there is a nextURI
				pending, otherwise this is false alarm due to de-sync between
				the two players
				*/
				if (strcmp(r, p->CurrentURI) && (p->State == PLAYING) && p->NextURI) {
					LOG_INFO("Detected URI change %s %s", p->CurrentURI, r);
					NFREE(p->CurrentURI);
					p->CurrentURI = malloc(strlen(r) + 1);
					strcpy(p->CurrentURI, r);
					ithread_mutex_unlock(&p->Mutex);
					sq_notify(p->SqueezeHandle, p, SQ_TRACK_CHANGE, 0, NULL);
				}
				else ithread_mutex_unlock(&p->Mutex);
			}
			NFREE(r);

			LOG_SDEBUG("Action complete : %i (cookie %d)", EventType, Cookie);

			if (Action->ErrCode != UPNP_E_SUCCESS) {
				LOG_ERROR("Error in action callback -- %d (cookie %d)",	Action->ErrCode, Cookie);
			}

			break;
		}
		default:
			break;
	}

	Cookie = Cookie;
	return 0;
}


/*----------------------------------------------------------------------------*/
#define SCAN_POLL (120*1000)
#define TRACK_POLL (1000)
#define STATE_POLL (500)
#define	SCAN_TIMEOUT (15)

void *TimerLoop(void *args)
{
	int i, rc, ScanPoll = 0;
	unsigned last;
	int	elapsed;
	struct sMR *p;
	char *Resp;

	last = gettime_ms();

	while (glTimerOn) {
		elapsed = gettime_ms() - last;
		ithread_mutex_lock(&glDeviceListMutex);
		p = glSQ2MRList;
		while (p)	{

			if (!p->on || (p->sqState == SQ_STOP && p->State == STOPPED)) {
				p = p->NextSQ;
				continue;
			}

			// get track position & CurrentURI
			p->TrackPoll += elapsed;
			if (p->TrackPoll > TRACK_POLL) {
				p->TrackPoll = 0;
				if (p->State != STOPPED && p->State != PAUSED) {
					AVTCallAction(p->Service[AVT_SRV_IDX].ControlURL, "GetPositionInfo", (void*) p->seqN++);
					AVTCallAction(p->Service[AVT_SRV_IDX].ControlURL, "GetMediaInfo", (void*) p->seqN++);
				}
			}

			// do polling as event is broken in many uPNP devices
			p->StatePoll += elapsed;
			if (p->StatePoll > STATE_POLL) {
				p->StatePoll = 0;
				AVTCallAction(p->Service[AVT_SRV_IDX].ControlURL, "GetTransportInfo", (void*) p->seqN++);
			}

#ifdef SUBSCRIBE_EVENT
			// renew rerevice subscribtion if needed
			for (i = 0; i < NB_SRV; i++) {
				struct sService *s = &p->Service[cSearchedSRV[i].idx];
				if (!s->TO--) {
					s->TO = 60;
					UpnpSubscribe(glControlPointHandle, s->EventURL, &s->TO, s->SID);
				}
			}
#endif

			p = p->NextSQ;
		}
		ithread_mutex_unlock(&glDeviceListMutex);

		// re-scan devices
		ScanPoll += elapsed;
		if (ScanPoll > SCAN_POLL) {
			ScanPoll = 0;
			// launch a new search for Media Render
			ithread_mutex_lock(&glDeviceListMutex);
			p = glDeviceList;
			while (p) {
				p->uPNPTimeOut = true;
				p = p->Next;
			}
			ithread_mutex_unlock(&glDeviceListMutex);
			rc = UpnpSearchAsync(glControlPointHandle, SCAN_TIMEOUT, MEDIA_RENDERER, NULL);
			if (UPNP_E_SUCCESS != rc) LOG_ERROR("Error sending search update%d", rc);

			if (loglevel >= lDEBUG) {
				ithread_mutex_lock(&glDeviceListMutex);
				p = glSQ2MRList;
				while (p)	{
					LOG_DEBUG("uPNP renderer [%s] [%s]", p->FriendlyName, p->UDN);
					for (i = 0; i < NB_SRV; i++) {
						if (strcmp(p->Service[i].Id, "")) {
							LOG_DEBUG("discovered service\n\t%s\n\t%s",	p->Service[i].ControlURL, p->Service[i].EventURL);
						}
					}
					p = p->NextSQ;
				}
				ithread_mutex_unlock(&glDeviceListMutex);
			}
		}

		last = gettime_ms();
		usleep(500000);
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
int CallbackEventHandler(Upnp_EventType EventType, void *Event, void *Cookie)
{
	LOG_SDEBUG("Event Handler : %i [%s]\n", EventType, uPNPEvent2String(EventType));

	switch ( EventType ) {
		case UPNP_DISCOVERY_ADVERTISEMENT_ALIVE:
		break;
		case UPNP_DISCOVERY_SEARCH_RESULT: {
			IXML_Document *DescDoc = NULL;
			int ret;
			struct Upnp_Discovery *d_event = (struct Upnp_Discovery *) Event;

			LOG_DEBUG("Answer to uPNP search", NULL);
			if (d_event->ErrCode != UPNP_E_SUCCESS) {
				LOG_DEBUG("Error in Discovery Callback -- %d", d_event->ErrCode);
			}
			ret = UpnpDownloadXmlDoc(d_event->Location, &DescDoc);
			if (ret != UPNP_E_SUCCESS) {
				LOG_DEBUG("Error obtaining description %s -- error = %d\n", d_event->Location, ret);
			}
			else AddMRDevice(DescDoc, d_event->Location, d_event->Expires);
			if (DescDoc) ixmlDocument_free(DescDoc);
			break;
		}
		case UPNP_DISCOVERY_SEARCH_TIMEOUT:	{
			struct sMR *p;

			LOG_INFO("uPNP search timeout", NULL);
			ithread_mutex_lock(&glDeviceListMutex);

			glSQ2MRList = NULL;
			p = glDeviceList;
			while (p)	{
				// create or re-create slimdevices and associated list
				if (!p->SqueezeHandle && p->Config.Enabled && !p->uPNPTimeOut)	{
					p->SqueezeHandle = sq_reserve_device(p, &sq_callback);
					sq_run_device(p->SqueezeHandle, *(p->Config.Name) ? p->Config.Name : p->FriendlyName, p->mac, &p->sq_config);
				}

				// uPNP device has gone dark ... remove it from slimdevice lists
				if (p->uPNPTimeOut && p->SqueezeHandle) {
					sq_delete_device(p->SqueezeHandle);
					p->SqueezeHandle = 0;
					p->on = false;
				}

				// finally, if confirmed active, insert device in the list
				if (p->SqueezeHandle) {
					if (glSQ2MRList) p->NextSQ = glSQ2MRList;
					else p->NextSQ = NULL;
					glSQ2MRList = p;
				}

				p  = p->Next;
			}

			ithread_mutex_unlock(&glDeviceListMutex);
			break;
		}
		case UPNP_EVENT_RECEIVED:
#ifdef SUBSCRIBE_EVENT
			HandleStateEvent(Event, Cookie);
#endif
			break;
		case UPNP_CONTROL_GET_VAR_COMPLETE:
			LOG_ERROR("Unexpected GetVarComplete", NULL);
			break;
		case UPNP_CONTROL_ACTION_COMPLETE: {
			struct Upnp_Action_Complete *Action = (struct Upnp_Action_Complete *)Event;
			struct sMR *p;
			char   *r;

			// only case where a action_complete lands here is for GetProtocolInfo
			ithread_mutex_lock(&glDeviceListMutex);
			// find device in global MR list, not only in SQ list
			p = glDeviceList;
			while (p) {
				int i;
				for (i = 0; i < NB_SRV && strcmp(p->Service[i].ControlURL, Action->CtrlUrl); i++);
				if (i < NB_SRV) break;
				p = p->Next;
			}
			if (!p) {
				ithread_mutex_unlock(&glDeviceListMutex);
				break;
			}

			r = XMLGetFirstDocumentItem(Action->ActionResult, "Sink");
			if (r) {
				LOG_DEBUG("[%p]: ProtocolInfo %s (cookie %d)", p, r, Cookie);
				ParseProtocolInfo(p, r);
			}
			ithread_mutex_unlock(&glDeviceListMutex);
			NFREE(r);
			break;
		}
		case UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE:
		case UPNP_CONTROL_ACTION_REQUEST:
		case UPNP_EVENT_SUBSCRIBE_COMPLETE:
		case UPNP_EVENT_UNSUBSCRIBE_COMPLETE:
		case UPNP_EVENT_RENEWAL_COMPLETE:
		case UPNP_EVENT_AUTORENEWAL_FAILED:
		case UPNP_EVENT_SUBSCRIPTION_EXPIRED:
		case UPNP_EVENT_SUBSCRIPTION_REQUEST:
		case UPNP_CONTROL_GET_VAR_REQUEST:
		break;
	}

	Cookie = Cookie;
	return 0;
}


/*----------------------------------------------------------------------------*/
int uPNPInitialize(char *IPaddress, unsigned int *Port)
{
	int rc;
	struct UpnpVirtualDirCallbacks VirtualDirCallbacks;

	ithread_mutex_init(&glDeviceListMutex, 0);


	UpnpSetLogLevel(UPNP_ALL);
	if (*IPaddress) rc = UpnpInit(IPaddress, *Port);
	else rc = UpnpInit(NULL, *Port);
	UpnpSetMaxContentLength(60000);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("uPNP init failed: %d\n", rc);
		UpnpFinish();
		return false;
	}

	if (!*IPaddress) {
		strcpy(IPaddress, UpnpGetServerIpAddress());
	}
	if (!*Port) {
		*Port = UpnpGetServerPort();
	}

	LOG_INFO("uPNP init success - %s:%u", IPaddress, *Port);

	rc = UpnpRegisterClient(CallbackEventHandler,
				&glControlPointHandle, &glControlPointHandle);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error registering ControlPoint: %d", rc);
		UpnpFinish();
		return false;
	}
	else {
		LOG_INFO("ControlPoint registered", NULL);
	}

	rc = UpnpEnableWebserver(true);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error initalizing WebServer: %d", rc);
		UpnpFinish();
		return false;
	}
	else {
		LOG_INFO("WebServer enabled", NULL);
	}

	rc = UpnpAddVirtualDir(glBaseVDIR);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error setting VirtualDir: %d", rc);
		UpnpFinish();
		return false;
	}
	else {
		LOG_INFO("VirtualDir set for Squeezelite", NULL);
	}

	VirtualDirCallbacks.get_info = WebGetInfo;
	VirtualDirCallbacks.open = WebOpen;
	VirtualDirCallbacks.read  = WebRead;
	VirtualDirCallbacks.seek = WebSeek;
	VirtualDirCallbacks.close = WebClose;
	VirtualDirCallbacks.write = WebWrite;
	rc = UpnpSetVirtualDirCallbacks(&VirtualDirCallbacks);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error registering VirtualDir callbacks: %d", rc);
		UpnpFinish();
		return false;
	}
	else {
		LOG_INFO("Callbacks registered for VirtualDir", NULL);
	}

	/* start a timer thread */
	ithread_create(&glTimerThread, NULL, TimerLoop, NULL);
	ithread_detach(glTimerThread);

	return true;
}

/*----------------------------------------------------------------------------*/
int uPNPTerminate(void)
{
//	ithread_cancel(glTimerThread);
	ithread_join(glTimerThread, NULL);
	UpnpUnRegisterClient(glControlPointHandle);
	UpnpEnableWebserver(false);
	UpnpFinish();

	return true;
}

/*----------------------------------------------------------------------------*/
int uPNPSearchMediaRenderer(void)
{
	int rc;

	/* search for (Media Render and wait 15s */
	rc = UpnpSearchAsync(glControlPointHandle, SCAN_TIMEOUT, MEDIA_RENDERER, NULL);

	if (UPNP_E_SUCCESS != rc) {
		LOG_ERROR("Error sending uPNP search request%d", rc);
		return false;
	}
	return true;
}

/*----------------------------------------------------------------------------*/
void SetVolumeCurve(struct sMR *Device)
{
	char buf[SQ_STR_LENGTH];
	char *p;
	int size, i = 0;
	int n = 0;

	strcpy(buf, Device->Config.VolumeCurve);
	size = strlen(buf);
	p = buf;
	do {
		char *q;

		p = strtok(p, ",");
		n += strlen(p) + 1;
		q = strtok(p, ":");
		Device->VolumeCurve[i].a = atol(q);
		Device->VolumeCurve[i].b = atol(q + strlen(q) + 1);
		p = buf + n;
		i++;
	} while (n < size);

	Device->VolumeCurve[i].a = 0xffffffff;
	Device->VolumeCurve[i].b = 0xff;
}


/*----------------------------------------------------------------------------*/
void AddMRDevice(IXML_Document *DescDoc, const char *location,	int expires)
{
	char *deviceType = NULL;
	char *friendlyName = NULL;
	char *URLBase = NULL;
	char *presURL = NULL;
	char *UDN = NULL;
	char UsedPresURL[200];
	char *ServiceId = NULL;
	char *EventURL = NULL;
	char *ControlURL = NULL;
	char *manufacturer = NULL;
	u8_t IPaddress[4];
	struct sMR *Device, *p;
	int rc = 1;

	ithread_mutex_lock(&glDeviceListMutex);

	/* Read key elements from description document */
	UDN = XMLGetFirstDocumentItem(DescDoc, "UDN");
	deviceType = XMLGetFirstDocumentItem(DescDoc, "deviceType");
	friendlyName = XMLGetFirstDocumentItem(DescDoc, "friendlyName");
	URLBase = XMLGetFirstDocumentItem(DescDoc, "URLBase");
	presURL = XMLGetFirstDocumentItem(DescDoc, "presentationURL");
	manufacturer = XMLGetFirstDocumentItem(DescDoc, "manufacturer");

	LOG_SDEBUG("UDN:\t%s\nDeviceType:\t%s\nFriendlyName:\t%s", UDN, deviceType, friendlyName);

	rc = UpnpResolveURL((URLBase ? URLBase : location), presURL, UsedPresURL);

	if (UPNP_E_SUCCESS != rc) {
		LOG_SDEBUG("Error generating presURL from %s + %s", URLBase, presURL);
	}

	if (strcmp(deviceType, MEDIA_RENDERER) == 0) {
		LOG_DEBUG("MediaRenderer UDN:\t%s\nDeviceType:\t%s\nFriendlyName:\t%s", UDN, deviceType, friendlyName);
	}

	/* Check if this device is already in the list */
	p = glDeviceList;
	while (p && strcmp(p->UDN, UDN)) p = p->Next;
	if (p) p->uPNPTimeOut = false;

	/*
	The device is already there, or this is a LMS device with upnp enabled so
	just update the advertisement timeout field
	*/
	if (!p && !strstr(manufacturer, cLogitech)) {
		int i;

		/* Create a new device node */
		Device = (struct sMR *) malloc(sizeof(struct sMR));
		memset(Device, 0, sizeof(struct sMR));

		Device->Magic = MAGIC;
		Device->uPNPTimeOut = false;
		Device->on = false;
		Device->macSize = 6;
		Device->SqueezeHandle = 0;
		strcpy(Device->UDN, UDN);
		strcpy(Device->DescDocURL, location);
		strcpy(Device->FriendlyName, friendlyName);
		strcpy(Device->Manufacturer, manufacturer);
		strcpy(Device->PresURL, UsedPresURL);
		ExtractIP(location, &Device->ip);

		if (SendARP(*((in_addr_t*) &Device->ip), INADDR_ANY, Device->mac, &Device->macSize)) {
			LOG_ERROR("[%p]: cannot get mac %s", Device, Device->FriendlyName);
			memset(Device->mac, 0, sizeof(Device->mac));
        }

		ithread_mutex_init(&Device->Mutex, 0);
		memcpy(&Device->Config, &glMRConfig, sizeof(tMRConfig));
		memcpy(&Device->sq_config, &glDeviceParam, sizeof(sq_dev_param_t));

		InitActionList(Device);
		for (i = 0 ; i < MAX_SRV; i++) strcpy(Device->Service[i].Id, "");

		/* find the AVTransport service */
		for (i = 0; i < NB_SRV; i++) {
			if (XMLFindAndParseService(DescDoc, location, cSearchedSRV[i].name, &ServiceId, &EventURL, &ControlURL)) {
				struct sService *s = &Device->Service[cSearchedSRV[i].idx];
				LOG_SDEBUG("\tservice [%s] %s, %s, %s", cSearchedSRV[i].name, ServiceId, EventURL, ControlURL);

				strncpy(s->Id, ServiceId, RESOURCE_LENGTH-1);
				strncpy(s->ControlURL, ControlURL, RESOURCE_LENGTH-1);
				strncpy(s->EventURL, EventURL, RESOURCE_LENGTH - 1);
				strcpy(s->Type, cSearchedSRV[i].name);
				s->TO = 60;
#ifdef SUBSCRIBE_EVENT
				UpnpSubscribe(glControlPointHandle, EventURL, &s->TO, s->SID);
#endif
				if (ServiceId) 	free(ServiceId);
				if (EventURL) 	free(EventURL);
				if (ControlURL) free(ControlURL);
			}
		}

		/* insert device in the list */
		Device->Next = NULL;
		if (glDeviceList) {
			struct sMR *p = glDeviceList;
			while (p->Next) p = p->Next;
			p->Next = Device;
		} else glDeviceList = Device;

		/* read parameters from config file  */
		LoadMRConfig(glConfigID, Device->UDN, &Device->Config, &Device->sq_config);
		SetVolumeCurve(Device);

		GetProtocolInfo(Device->Service[CNX_MGR_IDX].ControlURL, (void*) Device->seqN++);
	}

	ithread_mutex_unlock(&glDeviceListMutex);

	if (deviceType) 	free(deviceType);
	if (friendlyName) 	free(friendlyName);
	if (UDN) 			free(UDN);
	if (URLBase) 		free(URLBase);
	if (presURL)  		free(presURL);
}

/*----------------------------------------------------------------------------*/
void WatchDog(struct sMR *Device)
{
	if ((Device->sqState == SQ_PLAY && Device->State != PLAYING) ||
		(Device->sqState == SQ_STOP && Device->State != STOPPED)) {
		Device->Stalled++;
	}
	else Device->Stalled = 0;

	if (Device->Stalled >= 10) {
		sq_reset(Device->SqueezeHandle);
		Device->Stalled = 0;
	}
}

/*----------------------------------------------------------------------------*/
static bool Start(void)
{
	if (!uPNPInitialize(glIPaddress, &glPort)) return false;
	uPNPSearchMediaRenderer();
	return true;
}

static bool Stop(void)
{
	glTimerOn = false;
	uPNPTerminate();
	FlushMRList();
	return true;
}

/*---------------------------------------------------------------------------*/
static void sighandler(int signum) {
	glTimerOn = false;
	sq_stop();
	Stop();

	// remove ourselves in case above does not work, second SIGINT will cause non gracefull shutdown
	signal(signum, SIG_DFL);
}


/*---------------------------------------------------------------------------*/
bool ParseArgs(int argc, char **argv) {
	char *optarg = NULL;
	int optind = 1;
	int i;

#define MAXCMDLINE 256
	char cmdline[MAXCMDLINE] = "";

	for (i = 0; i < argc && (strlen(argv[i]) + strlen(cmdline) + 2 < MAXCMDLINE); i++) {
		strcat(cmdline, argv[i]);
		strcat(cmdline, " ");
	}

	while (optind < argc && strlen(argv[optind]) >= 2 && argv[optind][0] == '-') {
		char *opt = argv[optind] + 1;
		if (strstr("stxdf", opt) && optind < argc - 1) {
			optarg = argv[optind + 1];
			optind += 2;
		} else if (strstr("tz"
#if RESAMPLE
						  "uR"
#endif
#if DSD
						  "D"
#endif
		  , opt)) {
			optarg = NULL;
			optind += 1;
		}
		else {
			printf(usage);
			return false;
		}

		switch (opt[0]) {
		case 's':
			strcpy(glSQServer, optarg);
			break;
#if RESAMPLE
		case 'u':
		case 'R':
			if (optind < argc && argv[optind] && argv[optind][0] != '-') {
				gl_resample = argv[optind++];
			} else {
				gl_resample = "";
			}
			break;
#endif
		case 'f':
			glLogFile = optarg;
			break;
#if LINUX || FREEBSD
		case 'z':
			glDaemonize = true;
			break;
#endif
#if DSD
		case 'D':
			gl_dop = true;
			if (optind < argc && argv[optind] && argv[optind][0] != '-') {
				gl_dop_delay = atoi(argv[optind++]);
			}
			break;
#endif
		case 'd':
			{
				char *l = strtok(optarg, "=");
				char *v = strtok(NULL, "=");
				log_level new = lWARN;
				if (l && v) {
					if (!strcmp(v, "info"))   new = lINFO;
					if (!strcmp(v, "debug"))  new = lDEBUG;
					if (!strcmp(v, "sdebug")) new = lSDEBUG;
					if (!strcmp(l, "all") || !strcmp(l, "slimproto")) glLog.slimproto = new;
					if (!strcmp(l, "all") || !strcmp(l, "stream"))    glLog.stream = new;
					if (!strcmp(l, "all") || !strcmp(l, "decode"))    glLog.decode = new;
					if (!strcmp(l, "all") || !strcmp(l, "output"))    glLog.output = new;
					if (!strcmp(l, "all") || !strcmp(l, "web")) glLog.web = new;
					if (!strcmp(l, "all") || !strcmp(l, "upnp"))    glLog.upnp = new;
					if (!strcmp(l, "all") || !strcmp(l, "main"))    glLog.main = new;
					if (!strcmp(l, "all") || !strcmp(l, "sq2mr"))    glLog.sq2mr = new;

				}
				else {
					printf("usage");
					return false;
				}
			}
			break;
		case 't':
			printf(license);
			return false;
		default:
			break;
		}
	}

	return true;
}


/*----------------------------------------------------------------------------*/
/*																			  */
/*----------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	int i;
	char resp[20] = "";

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
#if defined(SIGQUIT)
	signal(SIGQUIT, sighandler);
#endif
#if defined(SIGHUP)
	signal(SIGHUP, sighandler);
#endif


	// first try to find a config file on the command line
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-x")) {
			strcpy(glConfigName, argv[i+1]);
		}
	}

	// load config from xml file
	glConfigID = (void*) LoadConfig(glConfigName, &glMRConfig, &glDeviceParam);

	// potentially overwrite with some cmdline parameters
	if (!ParseArgs(argc, argv)) exit(1);

	if (glLogFile) {
		if (!freopen(glLogFile, "a", stderr)) {
			fprintf(stderr, "error opening logfile %s: %s\n", glLogFile, strerror(errno));
		}
	}

#if LINUX || FREEBSD
	if (glDaemonize) {
		if (daemon(1, glLogFile ? 1 : 0)) {
			fprintf(stderr, "error daemonizing: %s\n", strerror(errno));
		}
	}
#endif

	if (strstr(glSQServer, "?")) sq_init(NULL, glMac, &glLog);
	else sq_init(glSQServer, glMac, &glLog);

	loglevel = glLog.sq2mr;
	uPNPLogLevel(glLog.upnp);
	WebServerLogLevel(glLog.web);
	AVTInit(glLog.sq2mr);
	MRutilInit(glLog.sq2mr);

	Start();

	while (strcmp(resp, "exit")) {

#if LINUX || FREEBSD
		if (!glDaemonize)
			scanf("%s", resp);
		else
			pause();
#else
		scanf("%s", resp);
#endif

		if (!strcmp(resp, "play")) {
			struct sMR *p;
			char name[128];

			scanf("%s", name);
			if (*name == '*') *name = '\0';
			ithread_mutex_lock(&glDeviceListMutex);
			p = glDeviceList;
			while (p)	{
				if (strstr(p->FriendlyName, name))
					AVTPlay (p->Service[AVT_SRV_IDX].ControlURL, NULL);
				p = p->Next;
			}
			ithread_mutex_unlock(&glDeviceListMutex);
		}

		if (!strcmp(resp, "pause")) {
			struct sMR *p;
			char name[128];

			scanf("%s", name);
			if (*name == '*') *name = '\0';
			ithread_mutex_lock(&glDeviceListMutex);
			p = glDeviceList;
			while (p)	{
				if (strstr(p->FriendlyName, name))
					AVTBasic (p->Service[AVT_SRV_IDX].ControlURL, "Pause", NULL);
				p = p->Next;
			}
			ithread_mutex_unlock(&glDeviceListMutex);
		}

		if (!strcmp(resp, "stop")) {
			struct sMR *p;
			char name[128];

			scanf("%s", name);
			if (*name == '*') *name = '\0';
			ithread_mutex_lock(&glDeviceListMutex);
			p = glDeviceList;
			while (p)	{
				if (strstr(p->FriendlyName, name))
					AVTBasic (p->Service[AVT_SRV_IDX].ControlURL, "Stop", NULL);
				p = p->Next;
			}
			ithread_mutex_unlock(&glDeviceListMutex);
		}

		if (!strcmp(resp, "set")) {
			char PlayURI[250];
			char name[128];
			struct sMR *p;

			scanf("%s", name);
			if (*name == '*') *name = '\0';
			ithread_mutex_lock(&glDeviceListMutex);
			p = glDeviceList;
			sprintf(PlayURI, "http://%s:%d/%s/__song__.mp3", glIPaddress, glPort, glBaseVDIR);

			while (p)	{
				if (strstr(p->FriendlyName, name))
				AVTSetURI (p->Service[AVT_SRV_IDX].ControlURL, PlayURI, NULL, NULL);
				p = p->Next;
			}

			ithread_mutex_unlock(&glDeviceListMutex);
		}

		if (!strcmp(resp, "sdbg"))	{
			char level[20];
			scanf("%s", level);
			stream_loglevel(debug2level(level));
		}

#if 0
		if (!strcmp(resp, "odbg"))	{
			char level[20];
			scanf("%s", level);
			output_loglevel(debug2level(level));
			output_mr_loglevel(debug2level(level));
		}
#endif                }

		if (!strcmp(resp, "pdbg"))	{
			char level[20];
			scanf("%s", level);
			slimproto_loglevel(debug2level(level));
		}

		if (!strcmp(resp, "wdbg"))	{
			char level[20];
			scanf("%s", level);
			WebServerLogLevel(debug2level(level));
		}

		if (!strcmp(resp, "mdbg"))	{
			char level[20];
			scanf("%s", level);
			main_loglevel(debug2level(level));
		}


		if (!strcmp(resp, "qdbg"))	{
			char level[20];
			scanf("%s", level);
			LOG_ERROR("Squeeze change log", NULL);
			loglevel = debug2level(level);
		}

		if (!strcmp(resp, "udbg"))	{
			char level[20];
			scanf("%s", level);
			uPNPLogLevel(debug2level(level));
		}

		 if (!strcmp(resp, "save"))	{
			char name[128];
			scanf("%s", name);
			SaveConfig(name);
		}
	}

	if (glConfigID) ixmlDocument_free(glConfigID);
	glTimerOn = false;
	sq_stop();
	Stop();

	return true;
}


static char usage[] =
 			VERSION "\n"
		   "See -t for license terms\n"
		   "Usage: [options]\n"
		   "  -s <server>[:<port>]\tConnect to specified server, otherwise uses autodiscovery to find server\n"
		   "  -x <config file>\tread config from file (default is ./config.xml)\n"
//		   "  -c <codec1>,<codec2>\tRestrict codecs to those specified, otherwise load all available codecs; known codecs: " CODECS "\n"
//		   "  -e <codec1>,<codec2>\tExplicitly exclude native support of one or more codecs; known codecs: " CODECS "\n"
		   "  -f <logfile>\t\tWrite debug to logfile\n"
		   "  -d <log>=<level>\tSet logging level, logs: all|slimproto|stream|decode|output|web|upnp|main|sq2mr, level: info|debug|sdebug\n"
#if RESAMPLE
		   "  -R -u [params]\tResample, params = <recipe>:<flags>:<attenuation>:<precision>:<passband_end>:<stopband_start>:<phase_response>,\n"
		   "  \t\t\t recipe = (v|h|m|l|q)(L|I|M)(s) [E|X], E = exception - resample only if native rate not supported, X = async - resample to max rate for device, otherwise to max sync rate\n"
		   "  \t\t\t flags = num in hex,\n"
		   "  \t\t\t attenuation = attenuation in dB to apply (default is -1db if not explicitly set),\n"
		   "  \t\t\t precision = number of bits precision (NB. HQ = 20. VHQ = 28),\n"
		   "  \t\t\t passband_end = number in percent (0dB pt. bandwidth to preserve. nyquist = 100%%),\n"
		   "  \t\t\t stopband_start = number in percent (Aliasing/imaging control. > passband_end),\n"
		   "  \t\t\t phase_response = 0-100 (0 = minimum / 50 = linear / 100 = maximum)\n"
#endif
#if DSD
		   "  -D [delay]\t\tOutput device supports DSD over PCM (DoP), delay = optional delay switching between PCM and DoP in ms\n"
#endif
#if LINUX || FREEBSD
		   "  -z \t\t\tDaemonize\n"
#endif
		   "  -t \t\t\tLicense terms\n"
		   "\n"
		   "Build options:"
#if LINUX
		   " LINUX"
#endif
#if WIN
		   " WIN"
#endif
#if OSX
		   " OSX"
#endif
#if FREEBSD
		   " FREEBSD"
#endif
#if EVENTFD
		   " EVENTFD"
#endif
#if SELFPIPE
		   " SELFPIPE"
#endif
#if WINEVENT
		   " WINEVENT"
#endif
#if RESAMPLE_MP
		   " RESAMPLE_MP"
#else
#if RESAMPLE
		   " RESAMPLE"
#endif
#endif
#if FFMPEG
		   " FFMPEG"
#endif
#if DSD
		   " DSD"
#endif
#if LINKALL
		   " LINKALL"
#endif
		   "\n\n";

static char license[] =
		   "This program is free software: you can redistribute it and/or modify\n"
		   "it under the terms of the GNU General Public License as published by\n"
		   "the Free Software Foundation, either version 3 of the License, or\n"
		   "(at your option) any later version.\n\n"
		   "This program is distributed in the hope that it will be useful,\n"
		   "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
		   "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
		   "GNU General Public License for more details.\n\n"
		   "You should have received a copy of the GNU General Public License\n"
		   "along with this program.  If not, see <http://www.gnu.org/licenses/>.\n\n"
#if DSD
		   "Contains dsd2pcm library Copyright 2009, 2011 Sebastian Gesemann which\n"
		   "is subject to its own license.\n\n"
#endif
	;


