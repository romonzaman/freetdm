/*
 * Copyright (c) 2007, Anthony Minessale II
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * * Neither the name of the original author; nor the names of any contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 * 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "openzap.h"
#include "zap_isdn.h"
#include "Q931.h"
#include "Q921.h"
#ifdef WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

#define LINE "--------------------------------------------------------------------------------"
//#define IODEBUG

/* helper macros */
#define ZAP_SPAN_IS_BRI(x)	((x)->trunk_type == ZAP_TRUNK_BRI || (x)->trunk_type == ZAP_TRUNK_BRI_PTMP)
#define ZAP_SPAN_IS_NT(x)	(((zap_isdn_data_t *)(x)->signal_data)->mode == Q921_NT)

static L2ULONG zap_time_now(void)
{
	return (L2ULONG)zap_current_time_in_ms();
}

static ZIO_CHANNEL_OUTGOING_CALL_FUNCTION(isdn_outgoing_call)
{
	zap_status_t status = ZAP_SUCCESS;
	zap_set_flag(zchan, ZAP_CHANNEL_OUTBOUND);
	zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_DIALING);
	return status;
}

static ZIO_CHANNEL_REQUEST_FUNCTION(isdn_channel_request)
{
	Q931mes_Generic *gen = (Q931mes_Generic *) caller_data->raw_data;
	Q931ie_BearerCap BearerCap;
	Q931ie_ChanID ChanID = { 0 };
	Q931ie_CallingNum CallingNum;
	Q931ie_CallingNum *ptrCallingNum;
	Q931ie_CalledNum CalledNum;
	Q931ie_CalledNum *ptrCalledNum;
	Q931ie_Display Display, *ptrDisplay;
	Q931ie_HLComp HLComp;			/* High-Layer Compatibility IE */
	Q931ie_SendComplete SComplete;		/* Sending Complete IE */
	Q931ie_ProgInd Progress;		/* Progress Indicator IE */
	zap_status_t status = ZAP_FAIL;
	zap_isdn_data_t *isdn_data = span->signal_data;
	int sanity = 60000;
	int codec  = 0;

	/*
	 * get codec type
	 */
	zap_channel_command(&span->channels[chan_id], ZAP_COMMAND_GET_NATIVE_CODEC, &codec);

	/*
	 * Q.931 Setup Message
	 */
	Q931InitMesGeneric(gen);
	gen->MesType = Q931mes_SETUP;
	gen->CRVFlag = 0;		/* outgoing call */

	/*
	 * Bearer Capability IE
	 */
	Q931InitIEBearerCap(&BearerCap);
	BearerCap.CodStand  = Q931_CODING_ITU;		/* ITU-T = 0, ISO/IEC = 1, National = 2, Network = 3 */
	BearerCap.ITC       = Q931_ITC_SPEECH;		/* Speech */
	BearerCap.TransMode = 0;			/* Circuit = 0, Packet = 1 */
	BearerCap.ITR       = Q931_ITR_64K;		/* 64k */
	BearerCap.Layer1Ident = 1;
	BearerCap.UIL1Prot = (codec == ZAP_CODEC_ALAW) ? Q931_UIL1P_G711A : Q931_UIL1P_G711U;	/* U-law = 2, A-law = 3 */
	gen->BearerCap = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &BearerCap);

	/*
	 * Channel ID IE
	 */
	Q931InitIEChanID(&ChanID);
	ChanID.IntType = ZAP_SPAN_IS_BRI(span) ? 0 : 1;		/* PRI = 1, BRI = 0 */

	if(!ZAP_SPAN_IS_NT(span)) {
		ChanID.PrefExcl = (isdn_data->opts & ZAP_ISDN_OPT_SUGGEST_CHANNEL) ? 0 : 1; /* 0 = preferred, 1 exclusive */
	} else {
		ChanID.PrefExcl = 1;	/* always exclusive in NT-mode */
	}

	if(ChanID.IntType) {
		ChanID.InfoChanSel = 1;				/* None = 0, See Slot = 1, Any = 3 */
		ChanID.ChanMapType = 3; 			/* B-Chan */
		ChanID.ChanSlot = (unsigned char)chan_id;
	} else {
		ChanID.InfoChanSel = (unsigned char)chan_id & 0x03;	/* None = 0, B1 = 1, B2 = 2, Any = 3 */
	}
	gen->ChanID = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &ChanID);

	/*
	 * Progress IE
	 */
	Q931InitIEProgInd(&Progress);
	Progress.CodStand = Q931_CODING_ITU;	/* 0 = ITU */
	Progress.Location = 0;  /* 0 = User, 1 = Private Network */
	Progress.ProgDesc = 3;	/* 1 = Not end-to-end ISDN */
	gen->ProgInd = Q931AppendIE((L3UCHAR *)gen, (L3UCHAR *)&Progress);

	/*
	 * Display IE
	 */
	Q931InitIEDisplay(&Display);
	Display.Size = Display.Size + (unsigned char)strlen(caller_data->cid_name);
	gen->Display = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &Display);			
	ptrDisplay = Q931GetIEPtr(gen->Display, gen->buf);
	zap_copy_string((char *)ptrDisplay->Display, caller_data->cid_name, strlen(caller_data->cid_name)+1);

	/*
	 * Calling Number IE
	 */
	Q931InitIECallingNum(&CallingNum);
	CallingNum.TypNum    = Q931_TON_UNKNOWN;
	CallingNum.NumPlanID = Q931_NUMPLAN_E164;
	CallingNum.PresInd   = Q931_PRES_ALLOWED;
	CallingNum.ScreenInd = Q931_SCREEN_USER_NOT_SCREENED;
	CallingNum.Size = CallingNum.Size + (unsigned char)strlen(caller_data->cid_num.digits);
	gen->CallingNum = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &CallingNum);			
	ptrCallingNum = Q931GetIEPtr(gen->CallingNum, gen->buf);
	zap_copy_string((char *)ptrCallingNum->Digit, caller_data->cid_num.digits, strlen(caller_data->cid_num.digits)+1);


	/*
	 * Called number IE
	 */
	Q931InitIECalledNum(&CalledNum);
	CalledNum.TypNum    = Q931_TON_UNKNOWN;
	CalledNum.NumPlanID = Q931_NUMPLAN_E164;
	CalledNum.Size = CalledNum.Size + (unsigned char)strlen(caller_data->ani.digits);
	gen->CalledNum = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &CalledNum);
	ptrCalledNum = Q931GetIEPtr(gen->CalledNum, gen->buf);
	zap_copy_string((char *)ptrCalledNum->Digit, caller_data->ani.digits, strlen(caller_data->ani.digits)+1);

	/*
	 * High-Layer Compatibility IE   (Note: Required for AVM FritzBox)
	 */
	Q931InitIEHLComp(&HLComp);
	HLComp.CodStand  = Q931_CODING_ITU;	/* ITU */
	HLComp.Interpret = 4;	/* only possible value */
	HLComp.PresMeth  = 1;   /* High-layer protocol profile */
	HLComp.HLCharID  = 1;	/* Telephony = 1, Fax G2+3 = 4, Fax G4 = 65 (Class I)/ 68 (Class II or III) */
	gen->HLComp = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &HLComp);

	/*
	 * Sending complete IE (or some NT stuff waits forever in Q.931 overlap dial state...)
	 */
	SComplete.IEId = Q931ie_SENDING_COMPLETE;
//	gen->SendComplete = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &SComplete);

	caller_data->call_state = ZAP_CALLER_STATE_DIALING;
	Q931Rx43(&isdn_data->q931, (L3UCHAR *) gen, gen->Size);
	
	isdn_data->outbound_crv[gen->CRV] = caller_data;
	//isdn_data->channels_local_crv[gen->CRV] = zchan;

	while(zap_running() && caller_data->call_state == ZAP_CALLER_STATE_DIALING) {
		zap_sleep(1);
		
		if (!--sanity) {
			caller_data->call_state = ZAP_CALLER_STATE_FAIL;
			break;
		}
	}
	isdn_data->outbound_crv[gen->CRV] = NULL;
	
	if (caller_data->call_state == ZAP_CALLER_STATE_SUCCESS) {
		zap_channel_t *new_chan = NULL;
		int fail = 1;
		
		new_chan = NULL;
		if (caller_data->chan_id < ZAP_MAX_CHANNELS_SPAN && caller_data->chan_id <= span->chan_count) {
			new_chan = &span->channels[caller_data->chan_id];
		}

		if (new_chan && (status = zap_channel_open_chan(new_chan) == ZAP_SUCCESS)) {
			if (zap_test_flag(new_chan, ZAP_CHANNEL_INUSE) || new_chan->state != ZAP_CHANNEL_STATE_DOWN) {
				if (new_chan->state == ZAP_CHANNEL_STATE_DOWN || new_chan->state >= ZAP_CHANNEL_STATE_TERMINATING) {
					int x = 0;
					zap_log(ZAP_LOG_WARNING, "Channel %d:%d ~ %d:%d is already in use waiting for it to become available.\n");
					
					for (x = 0; x < 200; x++) {
						if (!zap_test_flag(new_chan, ZAP_CHANNEL_INUSE)) {
							break;
						}
						zap_sleep(5);
					}
				}
				if (zap_test_flag(new_chan, ZAP_CHANNEL_INUSE)) {
					zap_log(ZAP_LOG_ERROR, "Channel %d:%d ~ %d:%d is already in use.\n",
							new_chan->span_id,
							new_chan->chan_id,
							new_chan->physical_span_id,
							new_chan->physical_chan_id
							);
					new_chan = NULL;
				}
			}

			if (new_chan && new_chan->state == ZAP_CHANNEL_STATE_DOWN) {
				isdn_data->channels_local_crv[gen->CRV] = new_chan;
				memset(&new_chan->caller_data, 0, sizeof(new_chan->caller_data));
				zap_set_flag(new_chan, ZAP_CHANNEL_OUTBOUND);
				zap_set_state_locked(new_chan, ZAP_CHANNEL_STATE_DIALING);
				switch(gen->MesType) {
				case Q931mes_ALERTING:
					new_chan->init_state = ZAP_CHANNEL_STATE_PROGRESS_MEDIA;
					break;
				case Q931mes_CONNECT:
					new_chan->init_state = ZAP_CHANNEL_STATE_UP;
					break;
				default:
					new_chan->init_state = ZAP_CHANNEL_STATE_PROGRESS;
					break;
				}

				fail = 0;
			} 
		}
		
		if (!fail) {
			*zchan = new_chan;
			return ZAP_SUCCESS;
		} else {
			Q931ie_Cause cause;
			gen->MesType = Q931mes_DISCONNECT;
			cause.IEId = Q931ie_CAUSE;
			cause.Size = sizeof(Q931ie_Cause);
			cause.CodStand  = 0;
			cause.Location = 1;
			cause.Recom = 1;
			//should we be casting here.. or do we need to translate value?
			cause.Value = (unsigned char) ZAP_CAUSE_WRONG_CALL_STATE;
			*cause.Diag = '\0';
			gen->Cause = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &cause);
			Q931Rx43(&isdn_data->q931, (L3UCHAR *) gen, gen->Size);

			if (gen->CRV) {
				Q931ReleaseCRV(&isdn_data->q931, gen->CRV);
			}
			
			if (new_chan) {
				zap_log(ZAP_LOG_CRIT, "Channel is busy\n");
			} else {
				zap_log(ZAP_LOG_CRIT, "Failed to open channel for new setup message\n");
			}
		}
	}
	
	*zchan = NULL;
	return ZAP_FAIL;

}

static L3INT zap_isdn_931_err(void *pvt, L3INT id, L3INT p1, L3INT p2)
{
	zap_log(ZAP_LOG_ERROR, "ERROR: [%s] [%d] [%d]\n", q931_error_to_name(id), p1, p2);
	return 0;
}

static L3INT zap_isdn_931_34(void *pvt, L2UCHAR *msg, L2INT mlen)
{
	zap_span_t *span = (zap_span_t *) pvt;
	zap_isdn_data_t *isdn_data = span->signal_data;
	Q931mes_Generic *gen = (Q931mes_Generic *) msg;
	int chan_id = 0;
	zap_channel_t *zchan = NULL;
	zap_caller_data_t *caller_data = NULL;

	if (Q931IsIEPresent(gen->ChanID)) {
		Q931ie_ChanID *chanid = Q931GetIEPtr(gen->ChanID, gen->buf);

		if(chanid->IntType)
			chan_id = chanid->ChanSlot;
		else
			chan_id = chanid->InfoChanSel;
	}

	assert(span != NULL);
	assert(isdn_data != NULL);
	
	zap_log(ZAP_LOG_DEBUG, "Yay I got an event! Type:[%02x] Size:[%d] CRV: %d (%#hx, CTX: %s)\n", gen->MesType, gen->Size, gen->CRV, gen->CRV, gen->CRVFlag ? "Terminator" : "Originator");

	if (gen->CRVFlag && (caller_data = isdn_data->outbound_crv[gen->CRV])) {
		if (chan_id) {
			caller_data->chan_id = chan_id;
		}

		switch(gen->MesType) {
		case Q931mes_STATUS:
		case Q931mes_CALL_PROCEEDING:
			break;
		case Q931mes_ALERTING:
		case Q931mes_PROGRESS:
		case Q931mes_CONNECT:
			{
				caller_data->call_state = ZAP_CALLER_STATE_SUCCESS;
			}
			break;
		default:
			caller_data->call_state = ZAP_CALLER_STATE_FAIL;
			break;
		}
	
		return 0;
	}

	if (gen->CRVFlag) {
		zchan = isdn_data->channels_local_crv[gen->CRV];
	} else {
		zchan = isdn_data->channels_remote_crv[gen->CRV];
	}

	zap_log(ZAP_LOG_DEBUG, "zchan %x source isdn_data->channels_%s_crv[%#hx]\n", zchan, gen->CRVFlag ? "local" : "remote", gen->CRV);


	if (gen->ProtDisc == 3) {
		switch(gen->MesType) {
		case Q931mes_SERVICE:
			{
				Q931ie_ChangeStatus *changestatus = Q931GetIEPtr(gen->ChangeStatus, gen->buf);
				if (zchan) {
					switch (changestatus->NewStatus) {
					case 0: /* change status to "in service" */
						{
							zap_clear_flag_locked(zchan, ZAP_CHANNEL_SUSPENDED);
							zap_log(ZAP_LOG_DEBUG, "Channel %d:%d in service\n", zchan->span_id, zchan->chan_id);
							zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_RESTART);
						}
						break;
					case 1: 
						{ /* change status to "maintenance" */
							zap_set_flag_locked(zchan, ZAP_CHANNEL_SUSPENDED);
							zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_SUSPENDED);
						}
						break;
					case 2:
						{ /* change status to "out of service" */
							zap_set_flag_locked(zchan, ZAP_CHANNEL_SUSPENDED);
							zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_SUSPENDED);
						}
						break;
					default: /* unknown */
						{
							break;
						}
					}
				}
			}
			break;
		default:
			break;
		}
	} else {
		switch(gen->MesType) {
		case Q931mes_RESTART:
			{
				if (chan_id) {
					zchan = &span->channels[chan_id];
				}
				if (zchan) {
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_RESTART);
				} else {
					uint32_t i;
					for (i = 0; i < span->chan_count; i++) {
						zap_set_state_locked((&span->channels[i]), ZAP_CHANNEL_STATE_RESTART);
					}
				}
			}
			break;
		case Q931mes_RELEASE:
		case Q931mes_RELEASE_COMPLETE:
			{
				const char *what = gen->MesType == Q931mes_RELEASE ? "Release" : "Release Complete";
				if (zchan) {
					if (zchan->state == ZAP_CHANNEL_STATE_TERMINATING || zchan->state == ZAP_CHANNEL_STATE_HANGUP) {
						if (gen->MesType == Q931mes_RELEASE) {
							zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP_COMPLETE);
						} else {
							zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_DOWN);
						}
					}
					else if((gen->MesType == Q931mes_RELEASE && zchan->state <= ZAP_CHANNEL_STATE_UP) ||
						(gen->MesType == Q931mes_RELEASE_COMPLETE && zchan->state == ZAP_CHANNEL_STATE_DIALING)) {

						/*
						 * Don't keep inbound channels open if the remote side hangs up before we answered
						 */
						Q931ie_Cause *cause = Q931GetIEPtr(gen->Cause, gen->buf);
						zap_sigmsg_t sig;
						zap_status_t status;

						memset(&sig, 0, sizeof(sig));
						sig.chan_id = zchan->chan_id;
						sig.span_id = zchan->span_id;
						sig.channel = zchan;
						sig.channel->caller_data.hangup_cause = (cause) ? cause->Value : ZAP_CAUSE_NORMAL_UNSPECIFIED;

						sig.event_id = ZAP_SIGEVENT_STOP;
						status = isdn_data->sig_cb(&sig);

						zap_log(ZAP_LOG_DEBUG, "Received %s in state %s, requested hangup for channel %d:%d\n", what, zap_channel_state2str(zchan->state), zchan->span_id, chan_id);
					}
					else {
						zap_log(ZAP_LOG_DEBUG, "Ignoring %s on channel %d\n", what, chan_id);
					}
				} else {
					zap_log(ZAP_LOG_CRIT, "Received %s with no matching channel %d\n", what, chan_id);
				}
			}
			break;
		case Q931mes_DISCONNECT:
			{
				if (zchan) {
					Q931ie_Cause *cause = Q931GetIEPtr(gen->Cause, gen->buf);
					zchan->caller_data.hangup_cause = cause->Value;
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_TERMINATING);
				} else {
					zap_log(ZAP_LOG_CRIT, "Received Disconnect with no matching channel %d\n", chan_id);
				}
			}
			break;
		case Q931mes_ALERTING:
			{
				if (zchan) {
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_PROGRESS_MEDIA);
				} else {
					zap_log(ZAP_LOG_CRIT, "Received Alerting with no matching channel %d\n", chan_id);
				}
			}
			break;
		case Q931mes_PROGRESS:
			{
				if (zchan) {
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_PROGRESS);
				} else {
					zap_log(ZAP_LOG_CRIT, "Received Progress with no matching channel %d\n", chan_id);
				}
			}
			break;
		case Q931mes_CONNECT:
			{
				if (zchan) {
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_UP);

					gen->MesType = Q931mes_CONNECT_ACKNOWLEDGE;
					gen->CRVFlag = 0;	/* outbound */
					Q931Rx43(&isdn_data->q931, (L3UCHAR *) gen, gen->Size);
				} else {
					zap_log(ZAP_LOG_CRIT, "Received Connect with no matching channel %d\n", chan_id);
				}
			}
			break;
		case Q931mes_SETUP:
			{
				Q931ie_CallingNum *callingnum = Q931GetIEPtr(gen->CallingNum, gen->buf);
				Q931ie_CalledNum *callednum = Q931GetIEPtr(gen->CalledNum, gen->buf);
				int fail = 1;
				uint32_t cplen = mlen;

				if(zchan && zchan == isdn_data->channels_remote_crv[gen->CRV]) {
					zap_log(ZAP_LOG_INFO, "Duplicate SETUP message(?) for Channel %d:%d ~ %d:%d in state %s [ignoring]\n",
									zchan->span_id,
									zchan->chan_id,
									zchan->physical_span_id,
									zchan->physical_chan_id,
									zap_channel_state2str(zchan->state));
					break;
				}
				
				zchan = NULL;
				if (chan_id < ZAP_MAX_CHANNELS_SPAN && chan_id <= span->chan_count) {
					zchan = &span->channels[chan_id];
				}

				if (zchan) {
					if (zap_test_flag(zchan, ZAP_CHANNEL_INUSE) || zchan->state != ZAP_CHANNEL_STATE_DOWN) {
						if (zchan->state == ZAP_CHANNEL_STATE_DOWN || zchan->state >= ZAP_CHANNEL_STATE_TERMINATING) {
							int x = 0;
							zap_log(ZAP_LOG_WARNING, "Channel %d:%d ~ %d:%d is already in use waiting for it to become available.\n",
									zchan->span_id,
									zchan->chan_id,
									zchan->physical_span_id,
									zchan->physical_chan_id);

							for (x = 0; x < 200; x++) {
								if (!zap_test_flag(zchan, ZAP_CHANNEL_INUSE)) {
									break;
								}
								zap_sleep(5);
							}
						}
						if (zap_test_flag(zchan, ZAP_CHANNEL_INUSE)) {
							zap_log(ZAP_LOG_ERROR, "Channel %d:%d ~ %d:%d is already in use.\n",
									zchan->span_id,
									zchan->chan_id,
									zchan->physical_span_id,
									zchan->physical_chan_id
									);
							zchan = NULL;
						}
					}

					if (zchan && zchan->state == ZAP_CHANNEL_STATE_DOWN) {
						isdn_data->channels_remote_crv[gen->CRV] = zchan;
						memset(&zchan->caller_data, 0, sizeof(zchan->caller_data));

						zap_set_string(zchan->caller_data.cid_num.digits, (char *)callingnum->Digit);
						zap_set_string(zchan->caller_data.cid_name, (char *)callingnum->Digit);
						zap_set_string(zchan->caller_data.ani.digits, (char *)callingnum->Digit);
						zap_set_string(zchan->caller_data.dnis.digits, (char *)callednum->Digit);

						zchan->caller_data.CRV = gen->CRV;
						if (cplen > sizeof(zchan->caller_data.raw_data)) {
							cplen = sizeof(zchan->caller_data.raw_data);
						}
						gen->CRVFlag = !(gen->CRVFlag);
						memcpy(zchan->caller_data.raw_data, msg, cplen);
						zchan->caller_data.raw_data_len = cplen;
						zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_RING);
						fail = 0;
					} 
				} 

				if (fail) {
					Q931ie_Cause cause;
					gen->MesType = Q931mes_DISCONNECT;
					gen->CRVFlag = 1;	/* inbound call */
					cause.IEId = Q931ie_CAUSE;
					cause.Size = sizeof(Q931ie_Cause);
					cause.CodStand = Q931_CODING_ITU;
					cause.Location = 1;
					cause.Recom = 1;
					//should we be casting here.. or do we need to translate value?
					cause.Value = (unsigned char) ZAP_CAUSE_WRONG_CALL_STATE;
					*cause.Diag = '\0';
					gen->Cause = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &cause);
					Q931Rx43(&isdn_data->q931, (L3UCHAR *) gen, gen->Size);

					if (gen->CRV) {
						Q931ReleaseCRV(&isdn_data->q931, gen->CRV);
					}

					if (zchan) {
						zap_log(ZAP_LOG_CRIT, "Channel is busy\n");
					} else {
						zap_log(ZAP_LOG_CRIT, "Failed to open channel for new setup message\n");
					}
					
				} else {
					Q931ie_ProgInd progress;

					/*
					 * Setup Progress indicator
					 */
					progress.IEId = Q931ie_PROGRESS_INDICATOR;
					progress.Size = sizeof(Q931ie_ProgInd);
					progress.CodStand = Q931_CODING_ITU;	/* ITU */ 
					progress.Location = 1;	/* private network serving the local user */
					progress.ProgDesc = 1;	/* call is not end-to-end isdn = 1, in-band information available = 8 */
					gen->ProgInd = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &progress);
				}
			}
			break;

		case Q931mes_CALL_PROCEEDING:
			{
				if (zchan) {
					zap_log(ZAP_LOG_CRIT, "Received CALL PROCEEDING message for channel %d\n", chan_id);
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_PROGRESS);
				} else {
					zap_log(ZAP_LOG_CRIT, "Received CALL PROCEEDING with no matching channel %d\n", chan_id);
				}
			}
			break;

		default:
			zap_log(ZAP_LOG_CRIT, "Received unhandled message %d (%#x)\n", (int)gen->MesType, (int)gen->MesType);
			break;
		}
	}

	return 0;
}

static int zap_isdn_921_23(void *pvt, Q921DLMsg_t ind, L2UCHAR tei, L2UCHAR *msg, L2INT mlen)
{
	int ret, offset = (ind == Q921_DL_DATA) ? 4 : 3;
	char bb[4096] = "";

	switch(ind) {
	case Q921_DL_DATA:
	case Q921_DL_UNIT_DATA:
		print_hex_bytes(msg + offset, mlen - offset, bb, sizeof(bb));
		zap_log(ZAP_LOG_DEBUG, "READ %d\n%s\n%s\n\n", (int)mlen - offset, LINE, bb);
	default:
		ret = Q931Rx23(pvt, ind, tei, msg, mlen);
		if (ret != 0)
			zap_log(ZAP_LOG_DEBUG, "931 parse error [%d] [%s]\n", ret, q931_error_to_name(ret));
		break;
	}

	return ((ret >= 0) ? 1 : 0);
}

static int zap_isdn_921_21(void *pvt, L2UCHAR *msg, L2INT mlen)
{
	zap_span_t *span = (zap_span_t *) pvt;
	zap_size_t len = (zap_size_t) mlen;
	zap_isdn_data_t *isdn_data = span->signal_data;

#ifdef IODEBUG
	char bb[4096] = "";
	print_hex_bytes(msg, len, bb, sizeof(bb));
	print_bits(msg, (int)len, bb, sizeof(bb), ZAP_ENDIAN_LITTLE, 0);
	zap_log(ZAP_LOG_DEBUG, "WRITE %d\n%s\n%s\n\n", (int)len, LINE, bb);

#endif

	assert(span != NULL);
	return zap_channel_write(isdn_data->dchan, msg, len, &len) == ZAP_SUCCESS ? 0 : -1;
}

static __inline__ void state_advance(zap_channel_t *zchan)
{
	Q931mes_Generic *gen = (Q931mes_Generic *) zchan->caller_data.raw_data;
	zap_isdn_data_t *isdn_data = zchan->span->signal_data;
	zap_sigmsg_t sig;
	zap_status_t status;

	zap_log(ZAP_LOG_DEBUG, "%d:%d STATE [%s]\n", 
			zchan->span_id, zchan->chan_id, zap_channel_state2str(zchan->state));

	memset(&sig, 0, sizeof(sig));
	sig.chan_id = zchan->chan_id;
	sig.span_id = zchan->span_id;
	sig.channel = zchan;

	switch (zchan->state) {
	case ZAP_CHANNEL_STATE_DOWN:
		{
			if (gen->CRV) {
				if(gen->CRVFlag) {
					isdn_data->channels_local_crv[gen->CRV] = NULL;
				} else {
					isdn_data->channels_remote_crv[gen->CRV] = NULL;
				}
				Q931ReleaseCRV(&isdn_data->q931, gen->CRV);
			}
			zap_channel_done(zchan);
		}
		break;
	case ZAP_CHANNEL_STATE_PROGRESS:
		{
			if (zap_test_flag(zchan, ZAP_CHANNEL_OUTBOUND)) {
				sig.event_id = ZAP_SIGEVENT_PROGRESS;
				if ((status = isdn_data->sig_cb(&sig) != ZAP_SUCCESS)) {
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP);
				}
			} else {
				gen->MesType = Q931mes_CALL_PROCEEDING;
				gen->CRVFlag = 1;	/* inbound */

				Q931Rx43(&isdn_data->q931, (void *)gen, gen->Size);
			}
		}
		break;
	case ZAP_CHANNEL_STATE_RING:
		{
			if (!zap_test_flag(zchan, ZAP_CHANNEL_OUTBOUND)) {
				sig.event_id = ZAP_SIGEVENT_START;
				if ((status = isdn_data->sig_cb(&sig) != ZAP_SUCCESS)) {
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP);
				}
			}
		}
		break;
	case ZAP_CHANNEL_STATE_RESTART:
		{
			zchan->caller_data.hangup_cause = ZAP_CAUSE_NORMAL_UNSPECIFIED;
			sig.event_id = ZAP_SIGEVENT_RESTART;
			status = isdn_data->sig_cb(&sig);
			zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_DOWN);
		}
		break;
	case ZAP_CHANNEL_STATE_PROGRESS_MEDIA:
		{
			if (zap_test_flag(zchan, ZAP_CHANNEL_OUTBOUND)) {
				sig.event_id = ZAP_SIGEVENT_PROGRESS_MEDIA;
				if ((status = isdn_data->sig_cb(&sig) != ZAP_SUCCESS)) {
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP);
				}
			} else {
				if (!zap_test_flag(zchan, ZAP_CHANNEL_OPEN)) {
					if (zap_channel_open_chan(zchan) != ZAP_SUCCESS) {
						zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP);
						return;
					}
				}
				gen->MesType = Q931mes_ALERTING;
				gen->CRVFlag = 1;	/* inbound call */
				Q931Rx43(&isdn_data->q931, (void *)gen, gen->Size);
			}
		}
		break;
	case ZAP_CHANNEL_STATE_UP:
		{
			if (zap_test_flag(zchan, ZAP_CHANNEL_OUTBOUND)) {
				sig.event_id = ZAP_SIGEVENT_UP;
				if ((status = isdn_data->sig_cb(&sig) != ZAP_SUCCESS)) {
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP);
				}
			} else {
				if (!zap_test_flag(zchan, ZAP_CHANNEL_OPEN)) {
					if (zap_channel_open_chan(zchan) != ZAP_SUCCESS) {
						zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP);
						return;
					}
				}
				gen->MesType = Q931mes_CONNECT;
				gen->BearerCap = 0;
				gen->CRVFlag = 1;	/* inbound call */
				Q931Rx43(&isdn_data->q931, (void *)gen, zchan->caller_data.raw_data_len);
			}
		}
		break;
	case ZAP_CHANNEL_STATE_DIALING:
		if (!(isdn_data->opts & ZAP_ISDN_OPT_SUGGEST_CHANNEL)) {
			Q931ie_BearerCap BearerCap;
			Q931ie_ChanID ChanID;
			Q931ie_CallingNum CallingNum;
			Q931ie_CallingNum *ptrCallingNum;
			Q931ie_CalledNum CalledNum;
			Q931ie_CalledNum *ptrCalledNum;
			Q931ie_Display Display, *ptrDisplay;
			Q931ie_HLComp HLComp;			/* High-Layer Compatibility IE */
			Q931ie_SendComplete SComplete;		/* Sending Complete IE */
			Q931ie_ProgInd Progress;		/* Progress Indicator IE */
			int codec  = 0;

			/*
			 * get codec type
			 */
			zap_channel_command(&zchan->span->channels[zchan->chan_id], ZAP_COMMAND_GET_NATIVE_CODEC, &codec);

			/*
			 * Q.931 Setup Message
			 */ 
			Q931InitMesGeneric(gen);
			gen->MesType = Q931mes_SETUP;
			gen->CRVFlag = 0;		/* outbound(?) */

			/*
			 * Bearer Capability IE
			 */
			Q931InitIEBearerCap(&BearerCap);
			BearerCap.CodStand  = Q931_CODING_ITU;	/* ITU-T = 0, ISO/IEC = 1, National = 2, Network = 3 */
			BearerCap.ITC       = Q931_ITC_SPEECH;	/* Speech */
			BearerCap.TransMode = 0;		/* Circuit = 0, Packet = 1 */
			BearerCap.ITR       = Q931_ITR_64K;	/* 64k = 16, Packet mode = 0 */
			BearerCap.Layer1Ident = 1;
			BearerCap.UIL1Prot = (codec == ZAP_CODEC_ALAW) ? 3 : 2;	/* U-law = 2, A-law = 3 */
			gen->BearerCap = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &BearerCap);

			/*
			 * ChannelID IE
			 */
			Q931InitIEChanID(&ChanID);
			ChanID.IntType = ZAP_SPAN_IS_BRI(zchan->span) ? 0 : 1;	/* PRI = 1, BRI = 0 */
			ChanID.PrefExcl = ZAP_SPAN_IS_NT(zchan->span) ? 1 : 0;  /* Exclusive in NT-mode = 1, Preferred otherwise = 0 */
			if(ChanID.IntType) {
				ChanID.InfoChanSel = 1;		/* None = 0, See Slot = 1, Any = 3 */
				ChanID.ChanMapType = 3;		/* B-Chan */
				ChanID.ChanSlot = (unsigned char)zchan->chan_id;
			} else {
				ChanID.InfoChanSel = (unsigned char)zchan->chan_id & 0x03;	/* None = 0, B1 = 1, B2 = 2, Any = 3 */
			}
			gen->ChanID = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &ChanID);

			/*
			 * Progress IE
			 */
			Q931InitIEProgInd(&Progress);
			Progress.CodStand = Q931_CODING_ITU;	/* 0 = ITU */
			Progress.Location = 0;  /* 0 = User, 1 = Private Network */
			Progress.ProgDesc = 3;	/* 1 = Not end-to-end ISDN */
			gen->ProgInd = Q931AppendIE((L3UCHAR *)gen, (L3UCHAR *)&Progress);

			/*
			 * Display IE
			 */			
			Q931InitIEDisplay(&Display);
			Display.Size = Display.Size + (unsigned char)strlen(zchan->caller_data.cid_name);
			gen->Display = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &Display);
			ptrDisplay = Q931GetIEPtr(gen->Display, gen->buf);
			zap_copy_string((char *)ptrDisplay->Display, zchan->caller_data.cid_name, strlen(zchan->caller_data.cid_name)+1);

			/*
			 * CallingNum IE
			 */ 
			Q931InitIECallingNum(&CallingNum);
			CallingNum.TypNum    = Q931_TON_UNKNOWN;
			CallingNum.NumPlanID = Q931_NUMPLAN_E164;
			CallingNum.PresInd   = Q931_PRES_ALLOWED;
			CallingNum.ScreenInd = Q931_SCREEN_USER_NOT_SCREENED;
			CallingNum.Size = CallingNum.Size + (unsigned char)strlen(zchan->caller_data.cid_num.digits);
			gen->CallingNum = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &CallingNum);
			ptrCallingNum = Q931GetIEPtr(gen->CallingNum, gen->buf);
			zap_copy_string((char *)ptrCallingNum->Digit, zchan->caller_data.cid_num.digits, strlen(zchan->caller_data.cid_num.digits)+1);

			/*
			 * CalledNum IE
			 */
			Q931InitIECalledNum(&CalledNum);
			CalledNum.TypNum    = Q931_TON_UNKNOWN;
			CalledNum.NumPlanID = Q931_NUMPLAN_E164;
			CalledNum.Size = CalledNum.Size + (unsigned char)strlen(zchan->caller_data.ani.digits);
			gen->CalledNum = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &CalledNum);
			ptrCalledNum = Q931GetIEPtr(gen->CalledNum, gen->buf);
			zap_copy_string((char *)ptrCalledNum->Digit, zchan->caller_data.ani.digits, strlen(zchan->caller_data.ani.digits)+1);

			/*
			 * High-Layer Compatibility IE   (Note: Required for AVM FritzBox)
			 */
			Q931InitIEHLComp(&HLComp);
			HLComp.CodStand  = Q931_CODING_ITU;	/* ITU */
			HLComp.Interpret = 4;	/* only possible value */
			HLComp.PresMeth  = 1;   /* High-layer protocol profile */
			HLComp.HLCharID  = Q931_HLCHAR_TELEPHONY;	/* Telephony = 1, Fax G2+3 = 4, Fax G4 = 65 (Class I)/ 68 (Class II or III) */   /* TODO: make accessible from user layer */
			gen->HLComp = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &HLComp);

			/*
			 * Sending complete IE (or some NT stuff waits forever in Q.931 overlap dial state...)
			 */
			SComplete.IEId = Q931ie_SENDING_COMPLETE;
//			gen->SendComplete = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &SComplete);

			Q931Rx43(&isdn_data->q931, (L3UCHAR *) gen, gen->Size);
			isdn_data->channels_local_crv[gen->CRV] = zchan;
		}
		break;
	case ZAP_CHANNEL_STATE_HANGUP_COMPLETE:
		{
			/* reply RELEASE with RELEASE_COMPLETE message */
			if(zchan->last_state == ZAP_CHANNEL_STATE_HANGUP) {
				gen->MesType = Q931mes_RELEASE_COMPLETE;

				Q931Rx43(&isdn_data->q931, (L3UCHAR *) gen, gen->Size);
			}
			zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_DOWN);
		}
		break;
	case ZAP_CHANNEL_STATE_HANGUP:
		{
			Q931ie_Cause cause;

			zap_log(ZAP_LOG_DEBUG, "Hangup: Direction %s\n", zap_test_flag(zchan, ZAP_CHANNEL_OUTBOUND) ? "Outbound" : "Inbound");

			gen->CRVFlag = zap_test_flag(zchan, ZAP_CHANNEL_OUTBOUND) ? 0 : 1;

			cause.IEId = Q931ie_CAUSE;
			cause.Size = sizeof(Q931ie_Cause);
			cause.CodStand = Q931_CODING_ITU;	/* ITU */
			cause.Location = 1;	/* private network */
			cause.Recom    = 1;	/* */

			/*
			 * BRI PTMP needs special handling here...
			 * TODO: cleanup / refine (see above)
			 */
			if (zchan->last_state == ZAP_CHANNEL_STATE_RING) {
				/*
				 * inbound call [was: number unknown (= not found in routing state)]
				 * (in Q.931 spec terms: Reject request)
				 */
				gen->MesType = Q931mes_RELEASE_COMPLETE;

				//cause.Value = (unsigned char) ZAP_CAUSE_UNALLOCATED;
				cause.Value = (unsigned char) zchan->caller_data.hangup_cause;
				*cause.Diag = '\0';
				gen->Cause = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &cause);
				Q931Rx43(&isdn_data->q931, (L3UCHAR *) gen, gen->Size);

				/* we're done, release channel */
				//zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP_COMPLETE);
				zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_DOWN);
			}
			else if (zchan->last_state <= ZAP_CHANNEL_STATE_PROGRESS) {
				/*
				 * just release all unanswered calls [was: inbound call, remote side hung up before we answered]
				 */
				gen->MesType = Q931mes_RELEASE;

				cause.Value = (unsigned char) zchan->caller_data.hangup_cause;
				*cause.Diag = '\0';
				gen->Cause = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &cause);
				Q931Rx43(&isdn_data->q931, (void *)gen, gen->Size);

				/* this will be triggered by the RELEASE_COMPLETE reply */
				/* zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP_COMPLETE); */
			}
			else {
				/*
				 * call connected, hangup
				 */
				gen->MesType = Q931mes_DISCONNECT;

				cause.Value = (unsigned char) zchan->caller_data.hangup_cause;
				*cause.Diag = '\0';
				gen->Cause = Q931AppendIE((L3UCHAR *) gen, (L3UCHAR *) &cause);
				Q931Rx43(&isdn_data->q931, (L3UCHAR *) gen, gen->Size);
			}
		}
		break;
	case ZAP_CHANNEL_STATE_TERMINATING:
		{
			zap_log(ZAP_LOG_DEBUG, "Terminating: Direction %s\n", zap_test_flag(zchan, ZAP_CHANNEL_OUTBOUND) ? "Outbound" : "Inbound");

			sig.event_id = ZAP_SIGEVENT_STOP;
			status = isdn_data->sig_cb(&sig);
			gen->MesType = Q931mes_RELEASE;
			gen->CRVFlag = zap_test_flag(zchan, ZAP_CHANNEL_OUTBOUND) ? 0 : 1;
			Q931Rx43(&isdn_data->q931, (void *)gen, gen->Size);
		}
	default:
		break;
	}
}

static __inline__ void check_state(zap_span_t *span)
{
    if (zap_test_flag(span, ZAP_SPAN_STATE_CHANGE)) {
        uint32_t j;
        zap_clear_flag_locked(span, ZAP_SPAN_STATE_CHANGE);
        for(j = 1; j <= span->chan_count; j++) {
            if (zap_test_flag((&span->channels[j]), ZAP_CHANNEL_STATE_CHANGE)) {
				zap_mutex_lock(span->channels[j].mutex);
                zap_clear_flag((&span->channels[j]), ZAP_CHANNEL_STATE_CHANGE);
                state_advance(&span->channels[j]);
                zap_channel_complete_state(&span->channels[j]);
				zap_mutex_unlock(span->channels[j].mutex);
            }
        }
    }
}


static __inline__ zap_status_t process_event(zap_span_t *span, zap_event_t *event)
{
	zap_sigmsg_t sig;
	zap_isdn_data_t *isdn_data = span->signal_data;

	memset(&sig, 0, sizeof(sig));
	sig.chan_id = event->channel->chan_id;
	sig.span_id = event->channel->span_id;
	sig.channel = event->channel;

	zap_log(ZAP_LOG_DEBUG, "EVENT [%s][%d:%d] STATE [%s]\n", 
			zap_oob_event2str(event->enum_id), event->channel->span_id, event->channel->chan_id, zap_channel_state2str(event->channel->state));

	switch(event->enum_id) {
	case ZAP_OOB_ALARM_TRAP:
		{
			sig.event_id = ZAP_OOB_ALARM_TRAP;
			if (event->channel->state != ZAP_CHANNEL_STATE_DOWN) {
				zap_set_state_locked(event->channel, ZAP_CHANNEL_STATE_RESTART);
			}
			zap_set_flag(event->channel, ZAP_CHANNEL_SUSPENDED);
			zap_channel_get_alarms(event->channel);
			isdn_data->sig_cb(&sig);
			zap_log(ZAP_LOG_WARNING, "channel %d:%d (%d:%d) has alarms [%s]\n", 
					event->channel->span_id, event->channel->chan_id, 
					event->channel->physical_span_id, event->channel->physical_chan_id, 
					event->channel->last_error);
		}
		break;
	case ZAP_OOB_ALARM_CLEAR:
		{
			sig.event_id = ZAP_OOB_ALARM_CLEAR;
			zap_clear_flag(event->channel, ZAP_CHANNEL_SUSPENDED);
			zap_channel_get_alarms(event->channel);
			isdn_data->sig_cb(&sig);
		}
		break;
	}

	return ZAP_SUCCESS;
}


static __inline__ void check_events(zap_span_t *span)
{
	zap_status_t status;

	status = zap_span_poll_event(span, 5);

	switch(status) {
	case ZAP_SUCCESS:
		{
			zap_event_t *event;
			while (zap_span_next_event(span, &event) == ZAP_SUCCESS) {
				if (event->enum_id == ZAP_OOB_NOOP) {
					continue;
				}
				if (process_event(span, event) != ZAP_SUCCESS) {
					break;
				}
			}
		}
		break;
	case ZAP_FAIL:
		{
			zap_log(ZAP_LOG_DEBUG, "Event Failure! %d\n", zap_running());
		}
		break;
	default:
		break;
	}
}

static void *zap_isdn_run(zap_thread_t *me, void *obj)
{
	zap_span_t *span = (zap_span_t *) obj;
	zap_isdn_data_t *isdn_data = span->signal_data;
	unsigned char buf[1024];
	zap_size_t len = sizeof(buf);
	int errs = 0;

#ifdef WIN32
    timeBeginPeriod(1);
#endif

	zap_log(ZAP_LOG_DEBUG, "ISDN thread starting.\n");

	Q921Start(&isdn_data->q921);

	while(zap_running() && zap_test_flag(isdn_data, ZAP_ISDN_RUNNING)) {
		zap_wait_flag_t flags = ZAP_READ;
		zap_status_t status = zap_channel_wait(isdn_data->dchan, &flags, 100);

		Q921TimerTick(&isdn_data->q921);
		Q931TimerTick(&isdn_data->q931);
		check_state(span);
		check_events(span);

		switch(status) {
		case ZAP_FAIL:
			{
				zap_log(ZAP_LOG_ERROR, "D-Chan Read Error!\n");
				snprintf(span->last_error, sizeof(span->last_error), "D-Chan Read Error!");
				if (++errs == 10) {
					isdn_data->dchan->state = ZAP_CHANNEL_STATE_UP;
					goto done;
				}
			}
			break;
		case ZAP_TIMEOUT:
			{
				errs = 0;
			}
			break;
		default:
			{
				errs = 0;
				if (flags & ZAP_READ) {
					len = sizeof(buf);
					if (zap_channel_read(isdn_data->dchan, buf, &len) == ZAP_SUCCESS) {
#ifdef IODEBUG
						char bb[4096] = "";
						print_hex_bytes(buf, len, bb, sizeof(bb));

						print_bits(buf, (int)len, bb, sizeof(bb), ZAP_ENDIAN_LITTLE, 0);
						zap_log(ZAP_LOG_DEBUG, "READ %d\n%s\n%s\n\n", (int)len, LINE, bb);
#endif
						
						Q921QueueHDLCFrame(&isdn_data->q921, buf, (int)len);
						Q921Rx12(&isdn_data->q921);
					}
				} else {
					zap_log(ZAP_LOG_DEBUG, "No Read FLAG!\n");
				}
			}
			break;
		}

	}
	
 done:

	zap_channel_close(&isdn_data->dchans[0]);
	zap_channel_close(&isdn_data->dchans[1]);
	zap_clear_flag(isdn_data, ZAP_ISDN_RUNNING);

#ifdef WIN32
    timeEndPeriod(1);
#endif

	zap_log(ZAP_LOG_DEBUG, "ISDN thread ended.\n");
	return NULL;
}

static ZIO_SIG_LOAD_FUNCTION(zap_isdn_init)
{
	Q931Initialize();

	Q921SetGetTimeCB(zap_time_now);
	Q931SetGetTimeCB(zap_time_now);

	return ZAP_SUCCESS;
}

static int q931_rx_32(void *pvt, Q921DLMsg_t ind, L3UCHAR tei, L3UCHAR *msg, L3INT mlen)
{
	int offset = 4;
	char bb[4096] = "";

	switch(ind) {
	case Q921_DL_UNIT_DATA:
		offset = 3;

	case Q921_DL_DATA:
		print_hex_bytes(msg + offset, mlen - offset, bb, sizeof(bb));
		zap_log(ZAP_LOG_DEBUG, "WRITE %d\n%s\n%s\n\n", (int)mlen - offset, LINE, bb);
		break;

	default:
		break;
	}

	return Q921Rx32(pvt, ind, tei, msg, mlen);
}

static int zap_isdn_q921_log(void *pvt, Q921LogLevel_t level, char *msg, L2INT size)
{
	int loglevel = ZAP_LOG_LEVEL_DEBUG;

	switch(level) {
	case Q921_LOG_DEBUG:
		loglevel = ZAP_LOG_LEVEL_DEBUG;
		break;

	case Q921_LOG_INFO:
		loglevel = ZAP_LOG_LEVEL_INFO;
		break;

	case Q921_LOG_NOTICE:
		loglevel = ZAP_LOG_LEVEL_NOTICE;
		break;

	case Q921_LOG_WARNING:
		loglevel = ZAP_LOG_LEVEL_WARNING;
		break;

	case Q921_LOG_ERROR:
		loglevel = ZAP_LOG_LEVEL_ERROR;
		break;

	default:
		return 0;
	}

	zap_log(__FILE__, "Q.921", __LINE__, loglevel, "%s", msg);

	return 0;
}

static L3INT zap_isdn_q931_log(void *pvt, Q931LogLevel_t level, char *msg, L3INT size)
{
	zap_log(__FILE__, "Q.931", __LINE__, ZAP_LOG_LEVEL_DEBUG, "%s", msg);
	return 0;
}

static zap_state_map_t isdn_state_map = {
	{
		{
			ZSD_OUTBOUND,
			ZSM_UNACCEPTABLE,
			{ZAP_ANY_STATE},
			{ZAP_CHANNEL_STATE_RESTART, ZAP_END}
		},
		{
			ZSD_OUTBOUND,
			ZSM_UNACCEPTABLE,
			{ZAP_CHANNEL_STATE_RESTART, ZAP_END},
			{ZAP_CHANNEL_STATE_DOWN, ZAP_END}
		},
		{
			ZSD_OUTBOUND,
			ZSM_UNACCEPTABLE,
			{ZAP_CHANNEL_STATE_DOWN, ZAP_END},
			{ZAP_CHANNEL_STATE_DIALING, ZAP_END}
		},
		{
			ZSD_OUTBOUND,
			ZSM_UNACCEPTABLE,
			{ZAP_CHANNEL_STATE_DIALING, ZAP_END},
			{ZAP_CHANNEL_STATE_PROGRESS_MEDIA, ZAP_CHANNEL_STATE_PROGRESS, ZAP_CHANNEL_STATE_UP, ZAP_CHANNEL_STATE_HANGUP, ZAP_CHANNEL_STATE_TERMINATING, ZAP_END}
		},
		{
			ZSD_OUTBOUND,
			ZSM_UNACCEPTABLE,
			{ZAP_CHANNEL_STATE_PROGRESS_MEDIA, ZAP_CHANNEL_STATE_PROGRESS, ZAP_END},
			{ZAP_CHANNEL_STATE_HANGUP, ZAP_CHANNEL_STATE_PROGRESS_MEDIA, ZAP_CHANNEL_STATE_TERMINATING, ZAP_CHANNEL_STATE_UP, ZAP_END}
		},
		{
			ZSD_OUTBOUND,
			ZSM_UNACCEPTABLE,
			{ZAP_CHANNEL_STATE_HANGUP, ZAP_CHANNEL_STATE_TERMINATING, ZAP_END},
			{ZAP_CHANNEL_STATE_HANGUP_COMPLETE, ZAP_CHANNEL_STATE_DOWN, ZAP_END}
		},
		{
			ZSD_OUTBOUND,
			ZSM_UNACCEPTABLE,
			{ZAP_CHANNEL_STATE_HANGUP_COMPLETE, ZAP_END},
			{ZAP_CHANNEL_STATE_DOWN, ZAP_END},
		},
		{
			ZSD_OUTBOUND,
			ZSM_UNACCEPTABLE,
			{ZAP_CHANNEL_STATE_UP, ZAP_END},
			{ZAP_CHANNEL_STATE_HANGUP, ZAP_CHANNEL_STATE_TERMINATING, ZAP_END}
		},

		/****************************************/
		{
			ZSD_INBOUND,
			ZSM_UNACCEPTABLE,
			{ZAP_ANY_STATE},
			{ZAP_CHANNEL_STATE_RESTART, ZAP_END}
		},
		{
			ZSD_INBOUND,
			ZSM_UNACCEPTABLE,
			{ZAP_CHANNEL_STATE_RESTART, ZAP_END},
			{ZAP_CHANNEL_STATE_DOWN, ZAP_END}
		},
		{
			ZSD_INBOUND,
			ZSM_UNACCEPTABLE,
			{ZAP_CHANNEL_STATE_DOWN, ZAP_END},
			{ZAP_CHANNEL_STATE_RING, ZAP_END}
		},
		{
			ZSD_INBOUND,
			ZSM_UNACCEPTABLE,
			{ZAP_CHANNEL_STATE_RING, ZAP_END},
			{ZAP_CHANNEL_STATE_HANGUP, ZAP_CHANNEL_STATE_TERMINATING, ZAP_CHANNEL_STATE_PROGRESS, ZAP_CHANNEL_STATE_PROGRESS_MEDIA, ZAP_CHANNEL_STATE_UP, ZAP_END}
		},
		{
			ZSD_INBOUND,
			ZSM_UNACCEPTABLE,
			{ZAP_CHANNEL_STATE_HANGUP, ZAP_CHANNEL_STATE_TERMINATING, ZAP_END},
			{ZAP_CHANNEL_STATE_HANGUP_COMPLETE, ZAP_CHANNEL_STATE_DOWN, ZAP_END},
		},
		{
			ZSD_INBOUND,
			ZSM_UNACCEPTABLE,
			{ZAP_CHANNEL_STATE_HANGUP_COMPLETE, ZAP_END},
			{ZAP_CHANNEL_STATE_DOWN, ZAP_END},
		},
		{
			ZSD_INBOUND,
			ZSM_UNACCEPTABLE,
			{ZAP_CHANNEL_STATE_PROGRESS, ZAP_CHANNEL_STATE_PROGRESS_MEDIA, ZAP_END},
			{ZAP_CHANNEL_STATE_HANGUP, ZAP_CHANNEL_STATE_PROGRESS_MEDIA, ZAP_CHANNEL_STATE_CANCEL, ZAP_CHANNEL_STATE_UP, ZAP_END},
		},
		{
			ZSD_INBOUND,
			ZSM_UNACCEPTABLE,
			{ZAP_CHANNEL_STATE_UP, ZAP_END},
			{ZAP_CHANNEL_STATE_HANGUP, ZAP_CHANNEL_STATE_TERMINATING, ZAP_END},
		},
		

	}
};

static zap_status_t zap_isdn_start(zap_span_t *span)
{
	zap_isdn_data_t *isdn_data = span->signal_data;
	zap_set_flag(isdn_data, ZAP_ISDN_RUNNING);
	return zap_thread_create_detached(zap_isdn_run, span);
}



static ZIO_SIG_CONFIGURE_FUNCTION(zap_isdn_configure_span)
{
	uint32_t i, x = 0;
	zap_channel_t *dchans[2] = {0};
	zap_isdn_data_t *isdn_data;
	char *var, *val;
	Q931Dialect_t dialect = Q931_Dialect_National;
	uint32_t opts = 0;

	if (span->signal_type) {
		snprintf(span->last_error, sizeof(span->last_error), "Span is already configured for signalling [%d].", span->signal_type);
		return ZAP_FAIL;
	}

	if (span->trunk_type >= ZAP_TRUNK_NONE) {
		snprintf(span->last_error, sizeof(span->last_error), "Unknown trunk type!");
		return ZAP_FAIL;
	}
	
	for(i = 1; i <= span->chan_count; i++) {
		if (span->channels[i].type == ZAP_CHAN_TYPE_DQ921) {
			if (x > 1) {
				snprintf(span->last_error, sizeof(span->last_error), "Span has more than 2 D-Channels!");
				return ZAP_FAIL;
			} else {
				if (zap_channel_open(span->span_id, i, &dchans[x]) == ZAP_SUCCESS) {
					zap_log(ZAP_LOG_DEBUG, "opening d-channel #%d %d:%d\n", x, dchans[x]->span_id, dchans[x]->chan_id);
					dchans[x]->state = ZAP_CHANNEL_STATE_UP;
					x++;
				}
			}
		}
	}

	if (!x) {
		snprintf(span->last_error, sizeof(span->last_error), "Span has no D-Channels!");
		return ZAP_FAIL;
	}

	isdn_data = malloc(sizeof(*isdn_data));
	assert(isdn_data != NULL);
	memset(isdn_data, 0, sizeof(*isdn_data));
	
	isdn_data->mode = Q931_TE;
	dialect = Q931_Dialect_National;
	
	while(var = va_arg(ap, char *)) {
		if (!strcasecmp(var, "mode")) {
			if (!(val = va_arg(ap, char *))) {
				break;
			}
			isdn_data->mode = strcasecmp(val, "net") ? Q931_TE : Q931_NT;
		} else if (!strcasecmp(var, "dialect")) {
			if (!(val = va_arg(ap, char *))) {
				break;
			}
			dialect = q931_str2Q931Dialect_type(val);
			if (dialect == Q931_Dialect_Count) {
				dialect = Q931_Dialect_National;
			}
		} else if (!strcasecmp(var, "opts")) {
			int *optp;
			if (!(optp = va_arg(ap, int *))) {
				break;
			}
			opts = isdn_data->opts = *optp;
		}
	}

	span->start = zap_isdn_start;
	isdn_data->sig_cb = sig_cb;
	isdn_data->dchans[0] = dchans[0];
	isdn_data->dchans[1] = dchans[1];
	isdn_data->dchan = isdn_data->dchans[0];
	
	Q921_InitTrunk(&isdn_data->q921,
				   0,
				   0,
				   isdn_data->mode,
				   span->trunk_type == ZAP_TRUNK_BRI_PTMP ? Q921_PTMP : Q921_PTP,
				   0,
				   zap_isdn_921_21,
				   (Q921Tx23CB_t)zap_isdn_921_23,
				   span,
				   &isdn_data->q931);

	Q921SetLogCB(&isdn_data->q921, &zap_isdn_q921_log, isdn_data);
	Q921SetLogLevel(&isdn_data->q921, Q921_LOG_DEBUG);
	
	Q931Api_InitTrunk(&isdn_data->q931,
					  dialect,
					  isdn_data->mode,
					  span->trunk_type,
					  zap_isdn_931_34,
					  (Q931Tx32CB_t)q931_rx_32,
					  zap_isdn_931_err,
					  &isdn_data->q921,
					  span);

	Q931SetLogCB(&isdn_data->q931, &zap_isdn_q931_log, isdn_data);
	Q931SetLogLevel(&isdn_data->q931, Q931_LOG_DEBUG);

	isdn_data->q931.autoRestartAck = 1;
//	isdn_data->q931.autoConnectAck = 1;
	isdn_data->q931.autoConnectAck = 0;
	isdn_data->q931.autoServiceAck = 1;
	span->signal_data = isdn_data;
	span->signal_type = ZAP_SIGTYPE_ISDN;
	span->outgoing_call = isdn_outgoing_call;


	if ((opts & ZAP_ISDN_OPT_SUGGEST_CHANNEL)) {
		span->channel_request = isdn_channel_request;
		span->suggest_chan_id = 1;
	}
	span->state_map = &isdn_state_map;

	return ZAP_SUCCESS;
}


zap_module_t zap_module = { 
	"isdn",
	NULL,
	NULL,
	zap_isdn_init,
	zap_isdn_configure_span,
	NULL
};


/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 expandtab:
 */