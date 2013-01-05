#include <string.h> //for memset
#include "nds.h"
#include "NeoIPC.h"
#include "NeoSystem7.h"
#include "NeoCpuZ80.h"
#include "NeoAudio.h"
#include "NeoAudioStream.h"
#include "NeoYM2610.h"

//how many times per second is neoYm2610Update called
#define YM2610_UPDATE_RATE (60 * Z80_TIMESLICE_PER_FRAME)

#define YM2610_PRESCALE (6*24)
#define YM2610_CLOCK (8*1000*1000)

#define YM2610_TIMER_PRESCALE (6*24)
#define YM2610_TIMER_DIV (YM2610_CLOCK/YM2610_TIMER_PRESCALE)

#define YM2610_SSG_STEP 0x8000
#define YM2610_SSG_SCALE (((YM2610_SSG_STEP * YM2610_UPDATE_RATE) / YM2610_CLOCK) * 8)

#define YM2610_SSG_CHANNEL0 11
#define YM2610_SSG_CHANNEL1 12
#define YM2610_SSG_CHANNEL2 13
#define YM2610_SSG_NOISE0 14
#define YM2610_SSG_NOISE1 15

//OPN->ST.TimerBase = 1.0 / (YM2610_CLOCK / (6*24));
//timer base = .000018

//SSG register ID
#define SSG_AFINE		(0)
#define SSG_ACOARSE		(1)
#define SSG_BFINE		(2)
#define SSG_BCOARSE		(3)
#define SSG_CFINE		(4)
#define SSG_CCOARSE		(5)
#define SSG_NOISEPER	(6)
#define SSG_ENABLE		(7)
#define SSG_AVOL		(8)
#define SSG_BVOL		(9)
#define SSG_CVOL		(10)
#define SSG_EFINE		(11)
#define SSG_ECOARSE		(12)
#define SSG_ESHAPE		(13)
#define SSG_PORTA		(14)
#define SSG_PORTB		(15)

#define ADPCM_SHIFT 16 // frequency step rate
#define ADPCMA_ADDRESS_SHIFT 8 // adpcm A address shift
#define ADPCMB_ADDRESS_SHIFT 8

#define ADPCMAFLAG_ACTIVE (1<<0)

static TYM2610Context g_ym2610;


	
	
/* calculate the volume->voltage conversion table */
/* The AY-3-8910 has 16 levels, in a logarithmic scale (3dB per step) */
/* The YM2149 still has 16 levels for the tone generators, but 32 for */
/* the envelope generator (1.5dB per step). */
//double out = 0x7f;
//int i;
//for(i = 31; i > 0; i--) {
//	g_ssgVolumeTable[i] = (u32)out;
//	out /= 1.188502227;	/* = 10 ^ (1.5/20) = 1.5dB */
//}
//table generated by above program
static const u8 g_ssgVolumeTable[32] = {
	0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 4, 4, 5, 6, 8, 9, 11, 13, 15,
	19, 22, 26, 31, 37, 45, 53, 63, 75, 89, 106, 127
};

static void ymInterruptSet(u32 flag)
{
	//NOTE - 2610 doesn't seem to have IRQ mask like some other yamaha chips
	g_ym2610.irqStatus |= flag;
	if(!g_ym2610.irq && g_ym2610.irqStatus) {
		g_ym2610.irq = true;
		neoZ80Irq();
	}
}

static void ymInterruptReset(u32 flag)
{
	g_ym2610.irqStatus &= ~flag;
	if(g_ym2610.irq && !g_ym2610.irqStatus) {
		g_ym2610.irq = false;
		neoZ80ClearIrq();
	}
}

static void ymTimerASet()
{
	g_ym2610.timerAValue = (1024 - g_ym2610.timerA);
	g_ym2610.timerATicks =
		(g_ym2610.timerAValue * YM2610_UPDATE_RATE) / YM2610_TIMER_DIV;
	if(g_ym2610.timerATicks == 0) g_ym2610.timerATicks = 1;
}

static void ymTimerBSet()
{
	g_ym2610.timerBValue = (256 - g_ym2610.timerB) << 4;
	g_ym2610.timerBTicks =
		(g_ym2610.timerBValue * YM2610_UPDATE_RATE) / YM2610_TIMER_DIV;
	if(g_ym2610.timerBTicks == 0) g_ym2610.timerBTicks = 1;
}

static void ymUpdateArriveEnd()
{
	u32 i;
	for(i = 0; i < 6; i++) {
		if(NEOIPC->adpcmaFinished[i]) {
			g_ym2610.adpcmArriveEnd |= g_ym2610.adpcma[i].flagMask;
			NEOIPC->adpcmaFinished[i] = 0;
		}
	}
	if(NEOIPC->adpcmaFinished[6]) {
		g_ym2610.adpcmArriveEnd |= g_ym2610.adpcmb.flagMask;
		NEOIPC->adpcmaFinished[6] = 0;
	}
}

static void ymOPNModeWrite(u16 r, u8 v)
{
	switch(r) {
	case 0x22: //LFO freq
		break;
	case 0x24: //timer A High
		g_ym2610.timerA = (g_ym2610.timerA & 0x03) | ((s32)v << 2);
		break;
	case 0x25: //timer A Low 2
		g_ym2610.timerA = (g_ym2610.timerA & 0x3fc) | (v & 0x03);
		break;
	case 0x26: // timer B
		g_ym2610.timerB = v;
		break;
	case 0x27: // mode, timer control
		/* b7 = CSM MODE */
		/* b6 = 3 slot mode */
		/* b5 = reset b */
		/* b4 = reset a */
		/* b3 = timer enable b */
		/* b2 = timer enable a */
		/* b1 = load b */
		/* b0 = load a */
		g_ym2610.mode = v;
		if(v & 0x20) ymInterruptReset(0x02);
		if(v & 0x10) ymInterruptReset(0x01);

		if(v & 0x02) {
			//set timer b
			if (g_ym2610.timerBValue == 0) {
				ymTimerBSet();
			}
		} else {
			//stop timer b
			if(g_ym2610.timerBValue != 0) {
				g_ym2610.timerBValue = 0;
				g_ym2610.timerBTicks = 0;
			}
		}

		if(v & 0x01) {
			//set timer a
			if(g_ym2610.timerAValue == 0) {
				ymTimerASet();
			}
		} else {
			//stop timer a
			if(g_ym2610.timerAValue != 0) {
				g_ym2610.timerAValue = 0;
				g_ym2610.timerATicks = 0;
			}
		}
		break;
	case 0x28: // key on / off
		break;
	}
}

TNeoAdpcmControl* ymGetChannelControl(u32 ch)
{
	const s32 pos9 = NEOIPC->adpcmQueuePos9[ch];
	s32 pos = NEOIPC->adpcmQueuePos7[ch];
	s32 pendingCount = pos - pos9;
	if(pendingCount < 0) pendingCount += NEO_COMMAND_QUEUE_SIZE;

	ASSERT(g_ym2610.queuePos[ch] == -1);

	if(pendingCount < NEO_COMMAND_QUEUE_SIZE - 1) {
		TNeoAdpcmControl* pControl = &NEOIPC->adpcmControl[ch][pos];
		pos++;
		if(pos >= NEO_COMMAND_QUEUE_SIZE) {
			pos = 0;
		}
		//update audio before checking time stamps
		neoAudioUpdate();
		pControl->command = NEOADPCM_NONE;
		pControl->audioFrame = NEOIPC->audioStreamCount;
		pControl->timeStamp = neoAudioGetTimestamp();
		//NEOIPC->adpcmQueuePos7[ch] = pos;
		g_ym2610.queuePos[ch] = pos;
		return pControl;
	}
	//can't add more commands until arm9 processes some
	return NULL;
}

void ymFinishChannelControl(u32 ch)
{
	ASSERT(g_ym2610.queuePos[ch] != -1);
	NEOIPC->adpcmQueuePos7[ch] = g_ym2610.queuePos[ch];
	g_ym2610.queuePos[ch] = -1;
}

static u32 ymSSGVolume(const TSSGChannel* pChannel)
{
	return SOUND_VOL(pChannel->volume);
}

static void ymSSGCalculateFreq(const TSSGChannel* pChannel)
{
	s32 freq;
	switch(pChannel->state) {
	case SSG_CHANNEL_TONE:
		freq = YM2610_CLOCK / 8 / pChannel->period;
		break;
	case SSG_CHANNEL_NOISE:
		freq = YM2610_CLOCK / 64 / g_ym2610.noise.period;
		break;
	case SSG_CHANNEL_NOISETONE:
		freq = (YM2610_CLOCK / 64 / pChannel->period);// +
			//(YM2610_CLOCK / 64 / g_ym2610.noise.period);
		break;
	//case SSG_CHANNEL_DISABLE:
	default:
		return;
	}
	neoAudioBufferWrite16((vu16*)&SCHANNEL_TIMER(pChannel->hwChannel),
		(s16)SOUND_FREQ(freq), 0xffff);
}

static bool ymSSGIsNoiseChannel(u32 hwch)
{
	return hwch == YM2610_SSG_NOISE0 || hwch == YM2610_SSG_NOISE1;
}

static u32 ymSSGAllocNoiseChannel()
{
	if((g_ym2610.noiseChannelUsed & 1) == 0) {
		g_ym2610.noiseChannelUsed |= 1;
		return YM2610_SSG_NOISE0;
	} else if((g_ym2610.noiseChannelUsed & 2) == 0) {
		g_ym2610.noiseChannelUsed |= 2;
		return YM2610_SSG_NOISE1;
	}
	return 0xff;
}

static void ymSSGFreeChannel(u32 hwch)
{
	if(ymSSGIsNoiseChannel(hwch)) {
		s32 i;
		//check to see if another ssg is waiting for a noise channel
		for(i = 0; i < 3; i++) {
			TSSGChannel* pChannel = &g_ym2610.ssg[i];
			if(pChannel->state >= SSG_CHANNEL_WAIT_NOISE) {
				//assign waiting ssg the newly freed channel
				pChannel->hwChannel = hwch;
				neoAudioBufferWrite32(&SCHANNEL_CR(pChannel->hwChannel),
					SCHANNEL_ENABLE | SOUND_FORMAT_PSG |
					SOUND_PAN(63) | ymSSGVolume(pChannel),
					0xffffffff);
				//waiting state -> not waiting state
				if(pChannel->state == SSG_CHANNEL_WAIT_NOISETONE) {
					pChannel->state = SSG_CHANNEL_NOISETONE;
				} else {
					pChannel->state = SSG_CHANNEL_NOISE;
				}
				ymSSGCalculateFreq(pChannel);
				break;
			}
		}
		if(i >= 3) {
			//no one was waiting for it, so free it
			g_ym2610.noiseChannelUsed &= ~(1 << (hwch - YM2610_SSG_NOISE0));
			neoAudioBufferWrite32(&SCHANNEL_CR(hwch), 0, 0xffffffff);
		}
	} else if(hwch >= YM2610_SSG_CHANNEL0 && hwch <= YM2610_SSG_CHANNEL2) {
		neoAudioBufferWrite32(&SCHANNEL_CR(hwch), 0, 0xffffffff);
	}
}

static void ymSSGWrite(u32 r, u8 v)
{
	TSSGChannel* pChannel;
	s32 ch;
	s32 i;

	switch(r) {
	case 0x00: case 0x02: case 0x04: //Channel A/B/C Fine Tune
	case 0x01: case 0x03: case 0x05: //Channel A/B/C Coarse
		ch = r >> 1;
		r &= ~1;
		g_ym2610.reg[r + 1] &= 0x0f;
		pChannel = &g_ym2610.ssg[ch];
		pChannel->period = (g_ym2610.reg[r] + 256 * g_ym2610.reg[r + 1]);
		if(pChannel->period == 0) {
			pChannel->period = 1;
		}
		ymSSGCalculateFreq(pChannel);
		break;
	case 0x06: //Noise percent
		g_ym2610.reg[SSG_NOISEPER] &= 0x1f;
		g_ym2610.noise.period = g_ym2610.reg[SSG_NOISEPER];
		if(g_ym2610.noise.period == 0) {
			g_ym2610.noise.period = 1;
		}
		for(i = 0; i < 3; i++) {
			pChannel = &g_ym2610.ssg[i];
			//update freq for all channels that include noise
			if(pChannel->state == SSG_CHANNEL_NOISETONE || pChannel->state == SSG_CHANNEL_NOISE) {
				ymSSGCalculateFreq(pChannel);
			}
		}
		break;
	case 0x07: //Enable
		for(i = 0; i < 3; i++) {
			pChannel = &g_ym2610.ssg[i];
			if(((v >> i) & 0x08) == 0) {
				//at least noise is on
				if(!ymSSGIsNoiseChannel(pChannel->hwChannel)) {
					//need to get rid of existing channel and allocate
					//ourselves a noise channel
					ymSSGFreeChannel(pChannel->hwChannel);
					pChannel->hwChannel = ymSSGAllocNoiseChannel();
				}
				if(ymSSGIsNoiseChannel(pChannel->hwChannel)) {
					//either successfuly allocated a noise channel,
					//or we already had one
					neoAudioBufferWrite32(&SCHANNEL_CR(pChannel->hwChannel),
						SCHANNEL_ENABLE | SOUND_FORMAT_PSG |
						SOUND_PAN(63) | ymSSGVolume(pChannel),
						0xffffffff);
					if(((v >> i) & 0x01) == 0) {
						//tone is on too
						pChannel->state = SSG_CHANNEL_NOISETONE;
					} else {
						//just noise
						pChannel->state = SSG_CHANNEL_NOISE;
					}
					ymSSGCalculateFreq(pChannel);
				} else {
					//ran out of channels
					if(((v >> i) & 1) == 0) {
						pChannel->state = SSG_CHANNEL_WAIT_NOISETONE;
					} else {
						pChannel->state = SSG_CHANNEL_WAIT_NOISE;
					}
					pChannel->hwChannel = 0xff;
				}
			} else if(((v >> i) & 0x01) == 0) {
				//just tone on
				if(ymSSGIsNoiseChannel(pChannel->hwChannel)) {
					//free noise channel
					ymSSGFreeChannel(pChannel->hwChannel);
				}
				//assign psg channel automatically
				pChannel->hwChannel = YM2610_SSG_CHANNEL0 + i;
				neoAudioBufferWrite32(&SCHANNEL_CR(pChannel->hwChannel),
					SCHANNEL_ENABLE | SOUND_FORMAT_PSG | (3 << 24) |
					SOUND_PAN(63) | ymSSGVolume(pChannel),
					0xffffffff);
				pChannel->state = SSG_CHANNEL_TONE;
				ymSSGCalculateFreq(pChannel);
			} else {
				//all off
				ymSSGFreeChannel(pChannel->hwChannel);
				pChannel->state = SSG_CHANNEL_DISABLE;
				pChannel->hwChannel = 0xff;
			}
		}
		break;
	case 0x08: case 0x09: case 0x0a: //Channel A/B/C Volume
		ch = r & 3;
		g_ym2610.reg[r] &= 0x1f;
		pChannel = &g_ym2610.ssg[ch];

		pChannel->envelopeEnable = g_ym2610.reg[r] & 0x10;
		if(pChannel->envelopeEnable) {
			//get volume from envelope (ignore volume reg)
			pChannel->volume = g_ym2610.ssgEnvelope.volume;
		} else if(g_ym2610.reg[r] != 0) {
			//get volume from table
			pChannel->volume = g_ssgVolumeTable[g_ym2610.reg[r] * 2 + 1];
		} else {
			//volume is set to zero (ignore table)
			pChannel->volume = 0;
		}
		neoAudioBufferWrite32(&SCHANNEL_CR(pChannel->hwChannel),
			ymSSGVolume(pChannel), SOUND_VOLUME_MASK);
		break;
	case SSG_EFINE: //Envelope Fine
	case SSG_ECOARSE: //Envelope Coarse
		g_ym2610.ssgEnvelope.count -= g_ym2610.ssgEnvelope.period;
		g_ym2610.ssgEnvelope.period =
			(g_ym2610.reg[SSG_EFINE] + 256 * g_ym2610.reg[SSG_ECOARSE]) * YM2610_SSG_SCALE;
		if(g_ym2610.ssgEnvelope.period == 0) {
			g_ym2610.ssgEnvelope.period = YM2610_SSG_SCALE / 2;
		}
		g_ym2610.ssgEnvelope.count += g_ym2610.ssgEnvelope.period;
		if(g_ym2610.ssgEnvelope.count <= 0) g_ym2610.ssgEnvelope.count = 1;
		break;
	case SSG_ESHAPE:	// Envelope Shapes
		g_ym2610.reg[SSG_ESHAPE] &= 0x0f;
		if(g_ym2610.reg[SSG_ESHAPE] & 0x04) {
			g_ym2610.ssgEnvelope.attack = 0x1f;
		} else {
			g_ym2610.ssgEnvelope.attack = 0;
		}

		if((g_ym2610.reg[SSG_ESHAPE] & 0x08) == 0) {
			//if Continue = 0, map the shape to the equivalent one which has Continue = 1
			g_ym2610.ssgEnvelope.hold = 1;
			g_ym2610.ssgEnvelope.alternate = g_ym2610.ssgEnvelope.attack;
		} else {
			g_ym2610.ssgEnvelope.hold = g_ym2610.reg[SSG_ESHAPE] & 0x01;
			g_ym2610.ssgEnvelope.alternate = g_ym2610.reg[SSG_ESHAPE] & 0x02;
		}
		g_ym2610.ssgEnvelope.count = g_ym2610.ssgEnvelope.period;
		g_ym2610.ssgEnvelope.index = 0x1f;
		g_ym2610.ssgEnvelope.holding = 0;
		g_ym2610.ssgEnvelope.volume = g_ssgVolumeTable[0x1f ^ g_ym2610.ssgEnvelope.attack];
		//update volume for all channels
		for(i = 0; i < 3; i++) {
			pChannel = &g_ym2610.ssg[i];
			if(pChannel->envelopeEnable && pChannel->volume != g_ym2610.ssgEnvelope.volume) {
				pChannel->volume = g_ym2610.ssgEnvelope.volume;
				neoAudioBufferWrite32(&SCHANNEL_CR(pChannel->hwChannel),
					ymSSGVolume(pChannel), SOUND_VOLUME_MASK);
			}
		}
		break;
	case SSG_PORTA:	//Port A
	case SSG_PORTB:	//Port B
		break;
	}
}

static void ymSSGEnvelopeProcess()
{
	//logic taken from mame fm.c implementation
	TSSGEnvelope* pEnv = &g_ym2610.ssgEnvelope;
	s32 i;

	if(pEnv->holding) {
		return;
	}
	pEnv->count -= YM2610_SSG_STEP;
	if(pEnv->count <= 0) {
		do {
			pEnv->index--;
			pEnv->count += pEnv->period;
		} while(pEnv->count <= 0);

		//check envelope current position
		if(pEnv->index < 0){
			if(pEnv->hold) {
				if(pEnv->alternate) pEnv->attack ^= 0x1f;
				pEnv->holding = 1;
				pEnv->index = 0;
			} else {
				//if count_env has looped an odd number of times (usually 1),
				//invert the output.
				if(pEnv->alternate && (pEnv->index & 0x20)) pEnv->attack ^= 0x1f;
				pEnv->index &= 0x1f;
			}
		}

		pEnv->volume = g_ssgVolumeTable[pEnv->index ^ pEnv->attack];
		
		for(i = 0; i < 3; i++) {
			TSSGChannel* pChannel = &g_ym2610.ssg[i];
			if(pChannel->envelopeEnable) {
				pChannel->volume = g_ym2610.ssgEnvelope.volume;
				neoAudioBufferWrite32(&SCHANNEL_CR(pChannel->hwChannel),
					ymSSGVolume(pChannel), SOUND_VOLUME_MASK);
			}
		}
	}
}

static void ymADPCMBCalculateFreq()
{
	//(u32)((float)(1 << ADPCM_SHIFT) * ((float)YM2610.OPN.ST.freqbase) / 3.0);
	//(U32)((double)DELTAT->delta * (DELTAT->freqbase) );
	//rate = 18500Hz, clock = 8Mhz, pres = 6*24
	//freqbase = ((double)OPN->ST.clock / OPN->ST.rate) / pres;
	u32 changed = 0;

	if(g_ym2610.adpcmb.delta > g_ym2610.adpcmb.lastDelta) {
		changed = g_ym2610.adpcmb.delta - g_ym2610.adpcmb.lastDelta;
	} else {
		changed = g_ym2610.adpcmb.lastDelta - g_ym2610.adpcmb.delta;
	}

	if(changed > 128) {
		u32 freq = ((u32)g_ym2610.adpcmb.delta * NEO_ADPCMB_RATE) >> 16;
		if(freq > NEO_ADPCMB_RATE) freq = NEO_ADPCMB_RATE;
		TNeoAdpcmControl* pControl = ymGetChannelControl(6);
		if(pControl) {
			pControl->frequency = (freq * 65536) / NEO_ADPCMA_RATE;
			pControl->command = NEOADPCM_FREQUENCY;
			g_ym2610.adpcmb.lastDelta = g_ym2610.adpcmb.delta;
			ymFinishChannelControl(6);
		}
	}
}

static void ymADPCMBCalculateVolume()
{
	//off, right, left, center
	static const u32 panTable[4] = {0, 0, 127, 64};

	u32 volume = g_ym2610.adpcmb.level;
	u32 pan = g_ym2610.adpcmb.pan;

	if(volume > 0x7f) volume = 0x7f;

	//g_ym2610.adpcmb.lastLevel = g_ym2610.adpcmb.level;
	//g_ym2610.adpcmb.lastPan = g_ym2610.adpcmb.pan;
		
	if(pan == 0) {
		//zero pan sets volume to zero
		neoAudioBufferWrite32(&SCHANNEL_CR(6), 0, SOUND_VOLUMEPAN_MASK);
		return;
	}
	neoAudioBufferWrite32(&SCHANNEL_CR(6),
		SOUND_VOL(volume) | SOUND_PAN(panTable[pan]), SOUND_VOLUMEPAN_MASK);
}

static void ymADPCMBWrite(u32 r, u8 v)
{
	u32 i;
	u16 v16;
	//NOTE - the ymdeltat unit can access cpu memory or external rom/ram
	//the version of the ymdeltat on the ym2610 only accesses external rom
	//so that's all I care about here
	//
	switch(r) {
	case 0x00:
		//NOTE - ignoring settings for external memory vs cpu
		if(v & 0x01) v = 0; //reset

		//start, rec, memory mode, repeat flag copy, reset(bit0)
		g_ym2610.adpcmb.portstate = v & (0x80|0x40|0x20|0x10|0x01); 

		if((v & 0x80) && g_ym2610.adpcmb.start < NEOIPC->audioRomSize) {
			//start
			ymUpdateArriveEnd();
			TNeoAdpcmControl* pControl = ymGetChannelControl(6);
			if(pControl) {
				u32 startAddr = g_ym2610.adpcmb.start;
				if(v & 0x10) {
					//set repeat flag in high bit of addr address
					startAddr |= 0x80000000;
				}
				pControl->startAddr = startAddr;
				pControl->command = NEOADPCM_START;
				ymFinishChannelControl(6);
			}
		} else if(!(v & 0x80)) {
			//stop
			ymUpdateArriveEnd();
			TNeoAdpcmControl* pControl = ymGetChannelControl(6);
			if(pControl) {
				pControl->command = NEOADPCM_STOP;
				ymFinishChannelControl(6);
			}
		}
		break;
	case 0x01:// L,R,-,-,SAMPLE,DA/AD,RAMTYPE,ROM
		//NOTE - ignoring settings for rom vs ram
		g_ym2610.adpcmb.pan = (v>>6) & 0x03;
		g_ym2610.adpcmb.control = v;
		ymADPCMBCalculateVolume();
		break;
	case 0x02: //Start Address L
	case 0x03: //Start Address H
		//NOTE - deltat registers mapped starting at ym2610 register index 0x10
		g_ym2610.adpcmb.start =
			((g_ym2610.reg[0x13] * 0x0100) | g_ym2610.reg[0x12]) << ADPCMB_ADDRESS_SHIFT;
		break;
	case 0x04: //Stop Address L
	case 0x05: //Stop Address H
		g_ym2610.adpcmb.end =
			((g_ym2610.reg[0x15] * 0x0100) | g_ym2610.reg[0x14]) << ADPCMB_ADDRESS_SHIFT;
		g_ym2610.adpcmb.end += (1 << ADPCMB_ADDRESS_SHIFT) - 1;
		TNeoAdpcmControl* pControl = ymGetChannelControl(6);
		if(pControl) {
			pControl->endAddr = g_ym2610.adpcmb.end;
			pControl->command = NEOADPCM_ENDADDR;
			ymFinishChannelControl(6);
		}
		break;
	case 0x06: //Prescale L (ADPCM and Record frq)
	case 0x07: //Prescale H
		break;
	case 0x08: //ADPCM data
		//NOTE - ignored (for now maybe)
		break;
	case 0x09: //DELTA-N L (ADPCM Playback Prescaler)
	case 0x0a: //DELTA-N H
		v16 = ((g_ym2610.reg[0x1a] * 0x0100) | g_ym2610.reg[0x19]);
		//don't need to send command if value is unchanged
		if(v16 == g_ym2610.adpcmb.delta) break;
		g_ym2610.adpcmb.delta = v16;
		ymADPCMBCalculateFreq();
		break;
	case 0x0b:	/* Output level control (volume, linear) */
		g_ym2610.adpcmb.level = v;
		ymADPCMBCalculateVolume();
		break;
	//NOTE - limit is not mapped to ym2610
	//case 0x0c: //Limit Address L
	//case 0x0d: //Limit Address H
	case 0x0c: //not actually part of the adpcmb unit, but mapped to its address space
		ymUpdateArriveEnd();
		for(i = 0; i < 6; i++) {
			g_ym2610.adpcma[i].flagMask = (~v) & (1 << i);
		}
		g_ym2610.adpcmb.flagMask = 0x80 & (~v);
		g_ym2610.adpcmArriveEnd &= (~v);
		break;
	}
}

static void ymADPCMACalculateVolume(s32 ch)
{
	//off, right, left, center
	static const u32 panTable[4] = {0, 127, 0, 64};

	u32 volume = g_ym2610.adpcma[ch].level + g_ym2610.adpcmaTotalLevel;
	u32 pan = g_ym2610.adpcma[ch].pan;

	//g_ym2610.adpcma[ch].lastLevel = g_ym2610.adpcma[ch].level;
	//g_ym2610.adpcma[ch].lastPan = g_ym2610.adpcma[ch].pan;
		
	if(volume >= 63 || pan == 0) {
		//zero volume or zero pan sets volume to zero
		neoAudioBufferWrite32(&SCHANNEL_CR(ch), 0, SOUND_VOLUMEPAN_MASK);
		return;
	}
	u32 multiply = 15 - (volume & 7); // so called 0.75 dB
	u32 shift = 1 + (volume >> 3);
	//adpcm decoder up shifts adpcm data up by 4
	//final output should be shifted up by 1
	//so we down shift our volume by 3 to compensate for this
	u32 hwVolume = (0x80 * multiply) >> (shift + 2);
	if(hwVolume > 0x7f) hwVolume = 0x7f;
	neoAudioBufferWrite32(&SCHANNEL_CR(ch),
		SOUND_VOL(hwVolume) | SOUND_PAN(panTable[pan]), SOUND_VOLUMEPAN_MASK);
}

static void ymADPCMAWrite(u32 r, u8 v)
{
	const u32 ch = r & 0x07;
	u32 i;

	switch(r) {
	case 0x00: /* DM,--,C5,C4,C3,C2,C1,C0 */
		ymUpdateArriveEnd();
		if(!(v & 0x80)) {
			//key on
			for(i = 0; i < 6; i++) {
				if(((v >> i) & 1) && g_ym2610.adpcma[i].start < NEOIPC->audioRomSize) {
					//start channel
					TNeoAdpcmControl* pControl = ymGetChannelControl(i);
					if(pControl) {
						pControl->startAddr = g_ym2610.adpcma[i].start;
						pControl->command = NEOADPCM_START;
						ymFinishChannelControl(i);
					}
				}
			}
		} else {
			//key off
			for(i = 0; i < 6; i++) {
				if(((v >> i) & 1)) {
					//stop channel
					TNeoAdpcmControl* pControl = ymGetChannelControl(i);
					if(pControl) {
						pControl->command = NEOADPCM_STOP;
						ymFinishChannelControl(i);
					}
				}
			}
		}
		break;
	case 0x01: /* B0-5 = TL */
		g_ym2610.adpcmaTotalLevel = (v & 0x3f) ^ 0x3f;
		for(i = 0; i < 6; i++) {
			ymADPCMACalculateVolume(i);
		}
		break;
	default:
		if(ch >= 0x06) {
			//invalid channel
			break;
		}
		switch(r & 0x38) {
		case 0x08:	/* B7=L, B6=R, B4-0=IL */
			//panning and local track volume
			g_ym2610.adpcma[ch].level = (v & 0x1f) ^ 0x1f;
			g_ym2610.adpcma[ch].pan = (v >> 6) & 0x03;
			ymADPCMACalculateVolume(ch);
			break;
		case 0x10:
		case 0x18:
			//start address
			g_ym2610.adpcma[ch].start =
				((g_ym2610.reg[0x118 + ch] * 0x100) +
				g_ym2610.reg[0x110 + ch]) << ADPCMA_ADDRESS_SHIFT;
			break;
		case 0x20:
		case 0x28:
			//end address
			g_ym2610.adpcma[ch].end =
				(((g_ym2610.reg[0x128 + ch] * 0x100) +
				g_ym2610.reg[0x120 + ch]) << ADPCMA_ADDRESS_SHIFT) +
				(1 << ADPCMA_ADDRESS_SHIFT) - 1;
			TNeoAdpcmControl* pControl = ymGetChannelControl(ch);
			if(pControl) {
				pControl->endAddr = g_ym2610.adpcma[ch].end;
				pControl->command = NEOADPCM_ENDADDR;
				ymFinishChannelControl(ch);
			}
			break;
		}
		break;
	}
}

u8 neoYM2610Read(u16 a)
{
	u32 addr = g_ym2610.address;

	switch(a & 3) {
	case 0: //status 0
		//high bit is busy flag...doesn't seem to be needed
		return g_ym2610.irqStatus & 0x03;
	case 1:
		if(addr < 16) return g_ym2610.reg[addr];
		else if(addr == 0xff) return 0x01;
		break;
	case 2: //adpcm status
		ymUpdateArriveEnd();
		return g_ym2610.adpcmArriveEnd;
	}
	return 0;
}

void neoYM2610Write(u16 a, u8 d)
{
	u32 addr;

	switch(a & 3) {
	case 0:
		g_ym2610.addrPort = 0;
		g_ym2610.address = d;
		break;
	case 1:
		if(g_ym2610.addrPort != 0) break;
		
		addr = g_ym2610.address;
		g_ym2610.reg[addr] = d;

		switch(addr & 0xf0) {
		case 0x00: //SSG
			ymSSGWrite(addr, d);
			break;
		case 0x10: //adpcmb
			ymADPCMBWrite(addr - 0x10, d);
			break;
		case 0x20: //OPN mode register
			ymOPNModeWrite(addr, d);
			break;
		default: //OPN register
			break;
		}
		break;
	case 2:
		g_ym2610.addrPort = 1;
		g_ym2610.address = d;
		break;
	case 3:
		if(g_ym2610.addrPort != 1) break;

		addr = g_ym2610.address;
		g_ym2610.reg[addr | 0x100] = d;

		if(addr < 0x30) { //ADPCM A
			ymADPCMAWrite(addr, d);
		} else { //OPN register
			//OPNWriteReg(OPN, addr | 0x100, v);
		}
		break;
	}
}

void neoYM2610Init()
{
	u32 i;

	memset(&g_ym2610, 0, sizeof(TYM2610Context));
	g_ym2610.adpcmaTotalLevel = 0x3f;
	ymOPNModeWrite(0x27, 0x30);
	for(i = 0; i < 6; i++) {
		g_ym2610.adpcma[i].flagMask = (1 << i);
	}
	g_ym2610.adpcmb.flagMask = 0x80;

	for(i = 0; i < 3; i++) {
		g_ym2610.ssg[i].hwChannel = 0xff;
		g_ym2610.ssg[i].state = SSG_CHANNEL_DISABLE;
	}
	for(i = 0; i < 14; i++) {
		ymSSGWrite(i, 0);
	}
	for(i = 0; i < 7; i++) {
		g_ym2610.queuePos[i] = -1;
	}
}

void neoYM2610Process()
{
	if(g_ym2610.timerATicks > 0) {
		g_ym2610.timerATicks--;
		if(g_ym2610.timerATicks == 0) {
			//generate interrupt if needed
			if(g_ym2610.mode & 0x04) ymInterruptSet(0x01);
			//reset timer
			ymTimerASet();
		}
	}
	if(g_ym2610.timerBTicks > 0) {
		g_ym2610.timerBTicks--;
		if(g_ym2610.timerBTicks == 0) {
			//generate interrupt if needed
			if(g_ym2610.mode & 0x08) ymInterruptSet(0x02);
			//reset timer
			ymTimerBSet();
		}
	}
	ymSSGEnvelopeProcess();
}

