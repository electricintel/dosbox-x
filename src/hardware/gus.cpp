/*
 *  Copyright (C) 2002-2015  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */


#include <string.h>
#include <iomanip>
#include <sstream>
#include "dosbox.h"
#include "inout.h"
#include "mixer.h"
#include "dma.h"
#include "pic.h"
#include "control.h"
#include "setup.h"
#include "shell.h"
#include "math.h"
#include "regs.h"
using namespace std;

//Extra bits of precision over normal gus
#define WAVE_BITS 2
#define WAVE_FRACT (9+WAVE_BITS)
#define WAVE_FRACT_MASK ((1 << WAVE_FRACT)-1)
#define WAVE_MSWMASK ((1 << (16+WAVE_BITS))-1)
#define WAVE_LSWMASK (0xffffffff ^ WAVE_MSWMASK)

//Amount of precision the volume has
#define RAMP_FRACT (10)
#define RAMP_FRACT_MASK ((1 << RAMP_FRACT)-1)

#define GUS_BASE myGUS.portbase
#define GUS_RATE myGUS.rate
#define LOG_GUS 0

// fixed panning table (avx)
static Bit16u const pantablePDF[16] = { 0, 13, 26, 41, 57, 72, 94, 116, 141, 169, 203, 244, 297, 372, 500, 4095 };
static bool gus_fixed_table = false;

Bit8u adlib_commandreg;
static MixerChannel * gus_chan;
static Bit8u const irqtable[8] = { 0, 2, 5, 3, 7, 11, 12, 15 };
static Bit8u const dmatable[8] = { 0, 1, 3, 5, 6, 7, 0, 0 };
static Bit8u GUSRam[1024*1024]; // 1024K of GUS Ram
static Bit32s AutoAmp = 512;
static Bit16u vol16bit[4096];
static Bit32u pantable[16];

class GUSChannels;
static void CheckVoiceIrq(void);

struct GFGus {
	Bit8u gRegSelect;
	Bit16u gRegData;
	Bit32u gDramAddr;
	Bit16u gCurChannel;

	Bit8u DMAControl;
	Bit16u dmaAddr;
	Bit8u dmaAddrOffset; /* bits 0-3 of the addr */
	Bit8u TimerControl;
	Bit8u SampControl;
	Bit8u mixControl;
	Bit8u ActiveChannels;
	Bit32u basefreq;

	struct GusTimer {
		float delay;
		Bit8u value;
		bool reached;
		bool raiseirq;
		bool masked;
		bool running;
	} timers[2];
	Bit32u rate;
	Bitu portbase;
	Bit32u memsize;
	Bit8u dma1;
	Bit8u dma2;

	Bit8u irq1;
	Bit8u irq2;

	bool irqenabled;
	bool ChangeIRQDMA;
	bool force_master_irq_enable;
	// IRQ status register values
	Bit8u IRQStatus;
	Bit32u ActiveMask;
	Bit8u IRQChan;
	Bit32u RampIRQ;
	Bit32u WaveIRQ;
} myGUS;

Bitu DEBUG_EnableDebugger(void);

static void GUS_DMA_Callback(DmaChannel * chan,DMAEvent event);

// Returns a single 16-bit sample from the Gravis's RAM
static INLINE Bit32s GetSample(Bit32u Delta, Bit32u CurAddr, bool eightbit) {
	Bit32u useAddr;
	Bit32u holdAddr;
	useAddr = CurAddr >> WAVE_FRACT;
	if (eightbit) {
		if (Delta >= (1 << WAVE_FRACT)) {
			Bit32s tmpsmall = (Bit8s)GUSRam[useAddr];
			return tmpsmall << 8;
		} else {
			// Interpolate
			Bit32s w1 = ((Bit8s)GUSRam[useAddr+0]) << 8;
			Bit32s w2 = ((Bit8s)GUSRam[useAddr+1]) << 8;
			Bit32s diff = w2 - w1;
			return (w1+((diff*(Bit32s)(CurAddr&WAVE_FRACT_MASK ))>>WAVE_FRACT));
		}
	} else {
		// Formula used to convert addresses for use with 16-bit samples
		holdAddr = useAddr & 0xc0000L;
		useAddr = useAddr & 0x1ffffL;
		useAddr = useAddr << 1;
		useAddr = (holdAddr | useAddr);

		if(Delta >= (1 << WAVE_FRACT)) {
			return (GUSRam[useAddr+0] | (((Bit8s)GUSRam[useAddr+1]) << 8));
		} else {
			// Interpolate
			Bit32s w1 = (GUSRam[useAddr+0] | (((Bit8s)GUSRam[useAddr+1]) << 8));
			Bit32s w2 = (GUSRam[useAddr+2] | (((Bit8s)GUSRam[useAddr+3]) << 8));
			Bit32s diff = w2 - w1;
			return (w1+((diff*(Bit32s)(CurAddr&WAVE_FRACT_MASK ))>>WAVE_FRACT));
		}
	}
}

class GUSChannels {
public:
	Bit32u WaveStart;
	Bit32u WaveEnd;
	Bit32u WaveAddr;
	Bit32u WaveAdd;
	Bit8u  WaveCtrl;
	Bit16u WaveFreq;

	Bit32u RampStart;
	Bit32u RampEnd;
	Bit32u RampVol;
	Bit32u RampAdd;
	Bit32u RampAddReal;

	Bit8u RampRate;
	Bit8u RampCtrl;

	Bit8u PanPot;
	Bit8u channum;
	Bit32u irqmask;
	Bit32u PanLeft;
	Bit32u PanRight;
	Bit32s VolLeft;
	Bit32s VolRight;

	GUSChannels(Bit8u num) { 
		channum = num;
		irqmask = 1 << num;
		WaveStart = 0;
		WaveEnd = 0;
		WaveAddr = 0;
		WaveAdd = 0;
		WaveFreq = 0;
		WaveCtrl = 3;
		RampRate = 0;
		RampStart = 0;
		RampEnd = 0;
		RampCtrl = 3;
		RampAdd = 0;
		RampAddReal = 0;
		RampVol = 0;
		VolLeft = 0;
		VolRight = 0;
		PanLeft = 0;
		PanRight = 0;
		PanPot = 0x7;
	};
	void WriteWaveFreq(Bit16u val) {
		WaveFreq = val;
		double frameadd = double(val >> 1)/512.0;		//Samples / original gus frame
		double realadd = (frameadd*(double)myGUS.basefreq/(double)GUS_RATE) * (double)(1 << WAVE_FRACT);
		WaveAdd = (Bit32u)realadd;
	}
	void WriteWaveCtrl(Bit8u val) {
		Bit32u oldirq=myGUS.WaveIRQ;
		WaveCtrl = val & 0x7f;
		if ((val & 0xa0)==0xa0) myGUS.WaveIRQ|=irqmask;
		else myGUS.WaveIRQ&=~irqmask;
		if (oldirq != myGUS.WaveIRQ) 
			CheckVoiceIrq();
	}
	INLINE Bit8u ReadWaveCtrl(void) {
		Bit8u ret=WaveCtrl;
		if (myGUS.WaveIRQ & irqmask) ret|=0x80;
		return ret;
	}
	void UpdateWaveRamp(void) { 
		WriteWaveFreq(WaveFreq);
		WriteRampRate(RampRate);
	}
	void WritePanPot(Bit8u val) {
		PanPot = val;
		PanLeft = pantable[(val & 0xf)];
		PanRight = pantable[0x0f-(val & 0xf)];
		UpdateVolumes();
	}
	Bit8u ReadPanPot(void) {
		return PanPot;
	}
	void WriteRampCtrl(Bit8u val) {
		Bit32u old=myGUS.RampIRQ;
		RampCtrl = val & 0x7f;
		if ((val & 0xa0)==0xa0) myGUS.RampIRQ|=irqmask;
		else myGUS.RampIRQ&=~irqmask;
		if (old != myGUS.RampIRQ) CheckVoiceIrq();
	}
	INLINE Bit8u ReadRampCtrl(void) {
		Bit8u ret=RampCtrl;
		if (myGUS.RampIRQ & irqmask) ret|=0x80;
		return ret;
	}
	void WriteRampRate(Bit8u val) {
		RampRate = val;
		double frameadd = (double)(RampRate & 63)/(double)(1 << (3*(val >> 6)));
		double realadd = (frameadd*(double)myGUS.basefreq/(double)GUS_RATE) * (double)(1 << RAMP_FRACT);
		RampAdd = (Bit32u)realadd;
	}
	INLINE void WaveUpdate(void) {
		if (WaveCtrl & 0x3) return;
		Bit32s WaveLeft;
		if (WaveCtrl & 0x40) {
			WaveAddr-=WaveAdd;
			WaveLeft=WaveStart-WaveAddr;
		} else {
			WaveAddr+=WaveAdd;
			WaveLeft=WaveAddr-WaveEnd;
		}
		if (WaveLeft<0) return;
		/* Generate an IRQ if needed */
		if (WaveCtrl & 0x20) {
			myGUS.WaveIRQ|=irqmask;
		}
		/* Check for not being in PCM operation */
		if (RampCtrl & 0x04) return;
		/* Check for looping */
		if (WaveCtrl & 0x08) {
			/* Bi-directional looping */
			if (WaveCtrl & 0x10) WaveCtrl^=0x40;
			WaveAddr = (WaveCtrl & 0x40) ? (WaveEnd-WaveLeft) : (WaveStart+WaveLeft);
		} else {
			WaveCtrl|=1;	//Stop the channel
			WaveAddr = (WaveCtrl & 0x40) ? WaveStart : WaveEnd;
		}
	}
	INLINE void UpdateVolumes(void) {
		Bit32s templeft=RampVol - PanLeft;
		templeft&=~(templeft >> 31);
		Bit32s tempright=RampVol - PanRight;
		tempright&=~(tempright >> 31);
		VolLeft=vol16bit[templeft >> RAMP_FRACT];
		VolRight=vol16bit[tempright >> RAMP_FRACT];
	}
	INLINE void RampUpdate(void) {
		/* Check if ramping enabled */
		if (RampCtrl & 0x3) return;
		Bit32s RampLeft;
		if (RampCtrl & 0x40) {
			RampVol-=RampAdd;
			RampLeft=RampStart-RampVol;
		} else {
			RampVol+=RampAdd;
			RampLeft=RampVol-RampEnd;
		}
		if (RampLeft<0) {
			UpdateVolumes();
			return;
		}
		/* Generate an IRQ if needed */
		if (RampCtrl & 0x20) {
			myGUS.RampIRQ|=irqmask;
		}
		/* Check for looping */
		if (RampCtrl & 0x08) {
			/* Bi-directional looping */
			if (RampCtrl & 0x10) RampCtrl^=0x40;
			RampVol = (RampCtrl & 0x40) ? (RampEnd-RampLeft) : (RampStart+RampLeft);
		} else {
			RampCtrl|=1;	//Stop the channel
			RampVol = (RampCtrl & 0x40) ? RampStart : RampEnd;
		}
		UpdateVolumes();
	}
	void generateSamples(Bit32s * stream,Bit32u len) {
		int i;
		Bit32s tmpsamp;
		bool eightbit;
		if (RampCtrl & WaveCtrl & 3) return;
		eightbit = ((WaveCtrl & 0x4) == 0);

		for(i=0;i<(int)len;i++) {
			// Get sample
			tmpsamp = GetSample(WaveAdd, WaveAddr, eightbit);
			// Output stereo sample
			stream[i<<1]+= tmpsamp * VolLeft;
			stream[(i<<1)+1]+= tmpsamp * VolRight;
			WaveUpdate();
			RampUpdate();
		}
	}
};

static GUSChannels *guschan[32] = {NULL};
static GUSChannels *curchan = NULL;

static INLINE void GUS_CheckIRQ(void);

static void GUS_TimerEvent(Bitu val);

static uint8_t GUS_reset_reg = 0;

static void GUSReset(void) {
	/* NTS: From the Ultrasound SDK:
	 *
	 *      Global Data Low (3X4) is either a 16-bit transfer, or the low half of a 16-bit transfer with 8-bit I/O.
	 *
	 *      Global Data High (3X5) is either an 8-bit transfer for one of the GF registers or the high part of a 16-bit wide register with 8-bit I/O.
	 *
	 *      Prior to 2015/12/29 DOSBox and DOSBox-X contained a programming error here where reset and master IRQ enable were handled from the
	 *      LOWER 8 bits, when the code should have been checking the UPPER 8 bits. Programming error #2 was the mis-interpetation of bit 0 (bit 8 of
	 *      the gRegData). According to the SDK, clearing bit 0 triggers RESET, setting bit 0 starts the card running again. The original code had
	 *      it backwards. */
	GUS_reset_reg = myGUS.gRegData >> 8;

	if ((myGUS.gRegData & 0x400) != 0x000 || myGUS.force_master_irq_enable)
		myGUS.irqenabled = true;
	else
		myGUS.irqenabled = false;

	GUS_CheckIRQ();

	LOG(LOG_MISC,LOG_DEBUG)("GUS reset with 0x%04X",myGUS.gRegData);
	if((myGUS.gRegData & 0x100) == 0x000) {
		// Stop all channels
		int i;
		for(i=0;i<32;i++) {
			guschan[i]->RampVol=0;
			guschan[i]->WriteWaveCtrl(0x1);
			guschan[i]->WriteRampCtrl(0x1);
			guschan[i]->WritePanPot(0x7);
		}

		// Reset
		adlib_commandreg = 85;
		myGUS.IRQStatus = 0;
		myGUS.RampIRQ = 0;
		myGUS.WaveIRQ = 0;
		myGUS.IRQChan = 0;

		myGUS.timers[0].delay = 0.080f;
		myGUS.timers[1].delay = 0.320f;
		myGUS.timers[0].value = 0xff;
		myGUS.timers[1].value = 0xff;
		myGUS.timers[0].masked = false;
		myGUS.timers[1].masked = false;
		myGUS.timers[0].raiseirq = false;
		myGUS.timers[1].raiseirq = false;
		myGUS.timers[0].reached = true;
		myGUS.timers[1].reached = true;
		myGUS.timers[0].running = false;
		myGUS.timers[1].running = false;

		PIC_RemoveEvents(GUS_TimerEvent);

		myGUS.ChangeIRQDMA = false;
		myGUS.DMAControl = 0x00;
		myGUS.mixControl = 0x0b;	// latches enabled by default LINEs disabled
		myGUS.TimerControl = 0x00;
		myGUS.SampControl = 0x00;
		myGUS.ActiveChannels = 14;
		myGUS.ActiveMask=0xffffffffU >> (32-myGUS.ActiveChannels);
		myGUS.basefreq = (Bit32u)((float)1000000/(1.619695497*(float)(myGUS.ActiveChannels)));

		myGUS.gCurChannel = 0;
		curchan = guschan[myGUS.gCurChannel];

		myGUS.dmaAddr = 0;
		myGUS.irqenabled = 0;
		myGUS.dmaAddrOffset = 0;
		myGUS.gDramAddr = 0;
		myGUS.gRegSelect = 0;
		myGUS.gRegData = 0;
	}

	GUS_CheckIRQ();
}

static INLINE void GUS_CheckIRQ(void) {
	bool dmaTC;
	bool otherIRQ;

	dmaTC = ((myGUS.IRQStatus & 0x80/*DMA TC IRQ*/)!=0) && ((myGUS.DMAControl & 0x20/*DMA IRQ Enable*/)!=0);
	otherIRQ = (myGUS.IRQStatus & 0x7F/*all except DMA TC IRQ pending*/);

	if (myGUS.irqenabled && (otherIRQ || dmaTC) && (myGUS.mixControl & 0x08)/*Enable latches*/)
		PIC_ActivateIRQ(myGUS.irq1);
	else
		PIC_DeActivateIRQ(myGUS.irq1);
}

static void CheckVoiceIrq(void) {
	myGUS.IRQStatus&=0x9f;
	Bitu totalmask=(myGUS.RampIRQ|myGUS.WaveIRQ) & myGUS.ActiveMask;
	if (!totalmask) return;
	if (myGUS.RampIRQ) myGUS.IRQStatus|=0x40;
	if (myGUS.WaveIRQ) myGUS.IRQStatus|=0x20;
	GUS_CheckIRQ();
	for (;;) {
		Bit32u check=(1 << myGUS.IRQChan);
		if (totalmask & check) return;
		myGUS.IRQChan++;
		if (myGUS.IRQChan>=myGUS.ActiveChannels) myGUS.IRQChan=0;
	}
}

static Bit16u ExecuteReadRegister(void) {
	Bit8u tmpreg;
//	LOG_MSG("Read global reg %x",myGUS.gRegSelect);
	switch (myGUS.gRegSelect) {
	case 0x41: // Dma control register - read acknowledges DMA IRQ
		tmpreg = myGUS.DMAControl & 0xbf;
		tmpreg |= (myGUS.IRQStatus & 0x80) >> 1;
		myGUS.IRQStatus&=0x7f;
		GUS_CheckIRQ();
		return (Bit16u)(tmpreg << 8);
	case 0x42:  // Dma address register
		return myGUS.dmaAddr;
	case 0x45:  // Timer control register.  Identical in operation to Adlib's timer
		return (Bit16u)(myGUS.TimerControl << 8);
		break;
	case 0x49:  // Dma sample register
		tmpreg = myGUS.DMAControl & 0xbf;
		tmpreg |= (myGUS.IRQStatus & 0x80) >> 1;
		return (Bit16u)(tmpreg << 8);
	case 0x4c:  // GUS reset register
		// FIXME: This makes logical sense, but does real hardware let you read this??
		// The Ultrasound SDK mentions writing the reg, but not necessarily the ability to read back.
		tmpreg = (GUS_reset_reg & ~0x4) | (myGUS.irqenabled ? 0x4 : 0x0);
		return (Bit16u)(tmpreg << 8);
	case 0x80: // Channel voice control read register
		if (curchan) return curchan->ReadWaveCtrl() << 8;
		else return 0x0300;

	case 0x82: // Channel MSB start address register
		if (curchan) return (Bit16u)(curchan->WaveStart >> (WAVE_BITS+16));
		else return 0x0000;
	case 0x83: // Channel LSW start address register
		if (curchan) return (Bit16u)(curchan->WaveStart >> WAVE_BITS);
		else return 0x0000;

	case 0x89: // Channel volume register
		if (curchan) return (Bit16u)((curchan->RampVol >> RAMP_FRACT) << 4);
		else return 0x0000;
	case 0x8a: // Channel MSB current address register
		if (curchan) return (Bit16u)(curchan->WaveAddr >> (WAVE_BITS+16));
		else return 0x0000;
	case 0x8b: // Channel LSW current address register
		if (curchan) return (Bit16u)(curchan->WaveAddr >> WAVE_BITS);
		else return 0x0000;

	case 0x8d: // Channel volume control register
		if (curchan) return curchan->ReadRampCtrl() << 8;
		else return 0x0300;
	case 0x8f: // General channel IRQ status register
		tmpreg=myGUS.IRQChan|0x20;
		Bit32u mask;
		mask=1 << myGUS.IRQChan;
        if (!(myGUS.RampIRQ & mask)) tmpreg|=0x40;
		if (!(myGUS.WaveIRQ & mask)) tmpreg|=0x80;
		myGUS.RampIRQ&=~mask;
		myGUS.WaveIRQ&=~mask;
		CheckVoiceIrq();
		return (Bit16u)(tmpreg << 8);
	default:
#if LOG_GUS
		LOG_MSG("Read Register num 0x%x", myGUS.gRegSelect);
#endif
		return myGUS.gRegData;
	}
}

static void GUS_TimerEvent(Bitu val) {
	if (!myGUS.timers[val].masked) myGUS.timers[val].reached=true;
	if (myGUS.timers[val].raiseirq) {
		myGUS.IRQStatus|=0x4 << val;
		GUS_CheckIRQ();
	}
	if (myGUS.timers[val].running) 
		PIC_AddEvent(GUS_TimerEvent,myGUS.timers[val].delay,val);
}

 
static void ExecuteGlobRegister(void) {
	int i;
//	if (myGUS.gRegSelect|1!=0x44) LOG_MSG("write global register %x with %x", myGUS.gRegSelect, myGUS.gRegData);
	switch(myGUS.gRegSelect) {
	case 0x0:  // Channel voice control register
		if(curchan) curchan->WriteWaveCtrl((Bit16u)myGUS.gRegData>>8);
		break;
	case 0x1:  // Channel frequency control register
		if(curchan) curchan->WriteWaveFreq(myGUS.gRegData);
		break;
	case 0x2:  // Channel MSW start address register
		if (curchan) {
			Bit32u tmpaddr = (Bit32u)(myGUS.gRegData & 0x1fff) << (16+WAVE_BITS);
			curchan->WaveStart = (curchan->WaveStart & WAVE_MSWMASK) | tmpaddr;
		}
		break;
	case 0x3:  // Channel LSW start address register
		if(curchan != NULL) {
			Bit32u tmpaddr = (Bit32u)(myGUS.gRegData) << WAVE_BITS;
			curchan->WaveStart = (curchan->WaveStart & WAVE_LSWMASK) | tmpaddr;
		}
		break;
	case 0x4:  // Channel MSW end address register
		if(curchan != NULL) {
			Bit32u tmpaddr = (Bit32u)(myGUS.gRegData & 0x1fff) << (16+WAVE_BITS);
			curchan->WaveEnd = (curchan->WaveEnd & WAVE_MSWMASK) | tmpaddr;
		}
		break;
	case 0x5:  // Channel MSW end address register
		if(curchan != NULL) {
			Bit32u tmpaddr = (Bit32u)(myGUS.gRegData) << WAVE_BITS;
			curchan->WaveEnd = (curchan->WaveEnd & WAVE_LSWMASK) | tmpaddr;
		}
		break;
	case 0x6:  // Channel volume ramp rate register
		if(curchan != NULL) {
			Bit8u tmpdata = (Bit16u)myGUS.gRegData>>8;
			curchan->WriteRampRate(tmpdata);
		}
		break;
	case 0x7:  // Channel volume ramp start register  EEEEMMMM
		if(curchan != NULL) {
			Bit8u tmpdata = (Bit16u)myGUS.gRegData >> 8;
			curchan->RampStart = tmpdata << (4+RAMP_FRACT);
		}
		break;
	case 0x8:  // Channel volume ramp end register  EEEEMMMM
		if(curchan != NULL) {
			Bit8u tmpdata = (Bit16u)myGUS.gRegData >> 8;
			curchan->RampEnd = tmpdata << (4+RAMP_FRACT);
		}
		break;
	case 0x9:  // Channel current volume register
		if(curchan != NULL) {
			Bit16u tmpdata = (Bit16u)myGUS.gRegData >> 4;
			curchan->RampVol = tmpdata << RAMP_FRACT;
			curchan->UpdateVolumes();
		}
		break;
	case 0xA:  // Channel MSW current address register
		if(curchan != NULL) {
			Bit32u tmpaddr = (Bit32u)(myGUS.gRegData & 0x1fff) << (16+WAVE_BITS);
			curchan->WaveAddr = (curchan->WaveAddr & WAVE_MSWMASK) | tmpaddr;
		}
		break;
	case 0xB:  // Channel LSW current address register
		if(curchan != NULL) {
			Bit32u tmpaddr = (Bit32u)(myGUS.gRegData) << (WAVE_BITS);
			curchan->WaveAddr = (curchan->WaveAddr & WAVE_LSWMASK) | tmpaddr;
		}
		break;
	case 0xC:  // Channel pan pot register
		if(curchan) curchan->WritePanPot((Bit16u)myGUS.gRegData>>8);
		break;
	case 0xD:  // Channel volume control register
		if(curchan) curchan->WriteRampCtrl((Bit16u)myGUS.gRegData>>8);
		break;
	case 0xE:  // Set active channel register
		myGUS.gRegSelect = myGUS.gRegData>>8;		//JAZZ Jackrabbit seems to assume this?
		myGUS.ActiveChannels = 1+((myGUS.gRegData>>8) & 63);
		if(myGUS.ActiveChannels < 14) myGUS.ActiveChannels = 14;
		if(myGUS.ActiveChannels > 32) myGUS.ActiveChannels = 32;
		myGUS.ActiveMask=0xffffffffU >> (32-myGUS.ActiveChannels);
		myGUS.basefreq = (Bit32u)((float)1000000/(1.619695497*(float)(myGUS.ActiveChannels)));
#if LOG_GUS
		LOG_MSG("GUS set to %d channels", myGUS.ActiveChannels);
#endif
		for (i=0;i<myGUS.ActiveChannels;i++) guschan[i]->UpdateWaveRamp();
		break;
	case 0x10:  // Undocumented register used in Fast Tracker 2
		break;
	case 0x41:  // Dma control register
		myGUS.DMAControl = (Bit8u)(myGUS.gRegData>>8);
		GetDMAChannel(myGUS.dma1)->Register_Callback(
			(myGUS.DMAControl & 0x1) ? GUS_DMA_Callback : 0);
		break;
	case 0x42:  // Gravis DRAM DMA address register
		myGUS.dmaAddr = myGUS.gRegData;
		break;
	case 0x43:  // MSB Peek/poke DRAM position
		myGUS.gDramAddr = (0xff0000 & myGUS.gDramAddr) | ((Bit32u)myGUS.gRegData);
		break;
	case 0x44:  // LSW Peek/poke DRAM position
		myGUS.gDramAddr = (0xffff & myGUS.gDramAddr) | ((Bit32u)myGUS.gRegData>>8) << 16;
		break;
	case 0x45:  // Timer control register.  Identical in operation to Adlib's timer
		myGUS.TimerControl = (Bit8u)(myGUS.gRegData>>8);
		myGUS.timers[0].raiseirq=(myGUS.TimerControl & 0x04)>0;
		if (!myGUS.timers[0].raiseirq) myGUS.IRQStatus&=~0x04;
		myGUS.timers[1].raiseirq=(myGUS.TimerControl & 0x08)>0;
		if (!myGUS.timers[1].raiseirq) myGUS.IRQStatus&=~0x08;
		GUS_CheckIRQ();
		break;
	case 0x46:  // Timer 1 control
		myGUS.timers[0].value = (Bit8u)(myGUS.gRegData>>8);
		myGUS.timers[0].delay = (0x100 - myGUS.timers[0].value) * 0.080f;
		break;
	case 0x47:  // Timer 2 control
		myGUS.timers[1].value = (Bit8u)(myGUS.gRegData>>8);
		myGUS.timers[1].delay = (0x100 - myGUS.timers[1].value) * 0.320f;
		break;
	case 0x49:  // DMA sampling control register
		myGUS.SampControl = (Bit8u)(myGUS.gRegData>>8);
		GetDMAChannel(myGUS.dma1)->Register_Callback(
			(myGUS.SampControl & 0x1)  ? GUS_DMA_Callback : 0);
		break;
	case 0x4c:  // GUS reset register
		GUSReset();
		break;
	default:
#if LOG_GUS
		LOG_MSG("Unimplemented global register %x -- %x", myGUS.gRegSelect, myGUS.gRegData);
#endif
		break;
	}
	return;
}


static Bitu read_gus(Bitu port,Bitu iolen) {
//	LOG_MSG("read from gus port %x",port);
	switch(port - GUS_BASE) {
	case 0x206:
		return myGUS.IRQStatus;
	case 0x208:
		Bit8u tmptime;
		tmptime = 0;
		if (myGUS.timers[0].reached) tmptime |= (1 << 6);
		if (myGUS.timers[1].reached) tmptime |= (1 << 5);
		if (tmptime & 0x60) tmptime |= (1 << 7);
		if (myGUS.IRQStatus & 0x04) tmptime|=(1 << 2);
		if (myGUS.IRQStatus & 0x08) tmptime|=(1 << 1);
		return tmptime;
	case 0x20a:
		return adlib_commandreg;
	case 0x302:
		return (Bit8u)myGUS.gCurChannel;
	case 0x303:
		return myGUS.gRegSelect;
	case 0x304:
		if (iolen==2) return ExecuteReadRegister() & 0xffff;
		else return ExecuteReadRegister() & 0xff;
	case 0x305:
		return ExecuteReadRegister() >> 8;
	case 0x307:
		if(myGUS.gDramAddr < myGUS.memsize) {
			return GUSRam[myGUS.gDramAddr];
		} else {
			return 0;
		}
	default:
#if LOG_GUS
		LOG_MSG("Read GUS at port 0x%x", port);
#endif
		break;
	}

	return 0xff;
}


static void write_gus(Bitu port,Bitu val,Bitu iolen) {
//	LOG_MSG("Write gus port %x val %x",port,val);
	switch(port - GUS_BASE) {
	case 0x200:
		myGUS.mixControl = (Bit8u)val;
		myGUS.ChangeIRQDMA = true;
		return;
	case 0x208:
		adlib_commandreg = (Bit8u)val;
		break;
	case 0x209:
//TODO adlib_commandreg should be 4 for this to work else it should just latch the value
		if (val & 0x80) {
			myGUS.timers[0].reached=false;
			myGUS.timers[1].reached=false;
			return;
		}
		myGUS.timers[0].masked=(val & 0x40)>0;
		myGUS.timers[1].masked=(val & 0x20)>0;
		if (val & 0x1) {
			if (!myGUS.timers[0].running) {
				PIC_AddEvent(GUS_TimerEvent,myGUS.timers[0].delay,0);
				myGUS.timers[0].running=true;
			}
		} else myGUS.timers[0].running=false;
		if (val & 0x2) {
			if (!myGUS.timers[1].running) {
				PIC_AddEvent(GUS_TimerEvent,myGUS.timers[1].delay,1);
				myGUS.timers[1].running=true;
			}
		} else myGUS.timers[1].running=false;
		break;
//TODO Check if 0x20a register is also available on the gus like on the interwave
	case 0x20b:
		if (!myGUS.ChangeIRQDMA) break;
		myGUS.ChangeIRQDMA=false;
		if (myGUS.mixControl & 0x40) {
			// IRQ configuration, only use low bits for irq 1
			if (irqtable[val & 0x7]) myGUS.irq1=irqtable[val & 0x7];
#if LOG_GUS
			LOG_MSG("Assigned GUS to IRQ %d", myGUS.irq1);
#endif
		} else {
			// DMA configuration, only use low bits for dma 1
			if (dmatable[val & 0x7]) myGUS.dma1=dmatable[val & 0x7];
#if LOG_GUS
			LOG_MSG("Assigned GUS to DMA %d", myGUS.dma1);
#endif
		}
		break;
	case 0x302:
		myGUS.gCurChannel = val & 31 ;
		curchan = guschan[myGUS.gCurChannel];
		break;
	case 0x303:
		myGUS.gRegSelect = (Bit8u)val;
		myGUS.gRegData = 0;
		break;
	case 0x304:
		if (iolen==2) {
			myGUS.gRegData=(Bit16u)val;
			ExecuteGlobRegister();
		} else myGUS.gRegData = (Bit16u)val;
		break;
	case 0x305:
		myGUS.gRegData = (Bit16u)((0x00ff & myGUS.gRegData) | val << 8);
		ExecuteGlobRegister();
		break;
	case 0x307:
		if(myGUS.gDramAddr < myGUS.memsize) GUSRam[myGUS.gDramAddr] = (Bit8u)val;
		break;
	default:
#if LOG_GUS
		LOG_MSG("Write GUS at port 0x%x with %x", port, val);
#endif
		break;
	}
}

/* TODO: Can we alter this to match ISA BUS DMA timing of the real hardware? */
static void GUS_DMA_Callback(DmaChannel * chan,DMAEvent event) {
	int step=0,docount=0;

	if (event == DMA_UNMASKED) {
		Bitu dmaaddr = (myGUS.dmaAddr << 4) + myGUS.dmaAddrOffset;
		Bitu dmalimit = myGUS.memsize;
		Bitu holdAddr;

		// FIXME: What does the GUS do if the DMA address goes beyond the end of memory?

		if (myGUS.DMAControl & 0x4) {
			// 16-bit wide DMA. The GUS SDK specifically mentions that 16-bit DMA is translated
			// to GUS RAM the same way you translate the play pointer. Eugh. But this allows
			// older demos to work properly even if you set the GUS DMA to a 16-bit channel (5)
			// instead of the usual 8-bit channel (1).
			holdAddr = dmaaddr & 0xc0000L;
			dmaaddr = dmaaddr & 0x1ffffL;
			dmaaddr = dmaaddr << 1;
			dmaaddr = (holdAddr | dmaaddr);
			dmalimit = ((dmaaddr & 0xc0000L) | 0x3FFFFL) + 1;
		}

		if (dmaaddr < dmalimit)
			docount = dmalimit - dmaaddr;

		if (docount > 0) {
			docount /= (chan->DMA16+1);
			if (docount > (chan->currcnt+1)) docount = chan->currcnt+1;

			if((myGUS.DMAControl & 0x2) == 0) {
				Bitu read=chan->Read(docount,&GUSRam[dmaaddr]);
				//Check for 16 or 8bit channel
				read*=(chan->DMA16+1);
				if((myGUS.DMAControl & 0x80) != 0) {
					//Invert the MSB to convert twos compliment form
					Bitu i;
					if((myGUS.DMAControl & 0x40) == 0) {
						// 8-bit data
						for(i=dmaaddr;i<(dmaaddr+read);i++) GUSRam[i] ^= 0x80;
					} else {
						// 16-bit data
						for(i=dmaaddr+1;i<(dmaaddr+read);i+=2) GUSRam[i] ^= 0x80;
					}
				}

				step = read;
			} else {
				//Read data out of UltraSound
				int wd = chan->Write(docount,&GUSRam[dmaaddr]);
				//Check for 16 or 8bit channel
				wd*=(chan->DMA16+1);

				step = wd;
			}
		}

		LOG(LOG_MISC,LOG_DEBUG)("GUS DMA transfer %lu bytes, GUS RAM address 0x%lx %u-bit",
			(unsigned long)step,(unsigned long)dmaaddr,myGUS.DMAControl & 0x4 ? 16 : 8);

		if (step > 0) {
			dmaaddr += (unsigned int)step;

			if (myGUS.DMAControl & 0x4) {
				holdAddr = dmaaddr & 0xc0000L;
				dmaaddr = dmaaddr & 0x3ffffL;
				dmaaddr = dmaaddr >> 1;
				dmaaddr = (holdAddr | dmaaddr);
			}

			myGUS.dmaAddr = dmaaddr >> 4;
			myGUS.dmaAddrOffset = dmaaddr & 0xF;
		}
	}

	if (event == DMA_UNMASKED || event == DMA_REACHED_TC) {
		if (chan->tcount) {
			LOG(LOG_MISC,LOG_DEBUG)("GUS DMA: terminal count reached. DMAControl=0x%02x",myGUS.DMAControl);

			/* Raise the TC irq if needed */
			myGUS.dmaAddrOffset = 0;
			myGUS.IRQStatus |= 0x80;
			GUS_CheckIRQ();
			chan->Register_Callback(0);
		}
	}
}

static void GUS_CallBack(Bitu len) {
	memset(&MixTemp,0,len*8);
	Bitu i;
	Bit16s * buf16 = (Bit16s *)MixTemp;
	Bit32s * buf32 = (Bit32s *)MixTemp;

	if ((GUS_reset_reg & 0x03/*DAC enable | !master reset*/) == 0x03) {
		for(i=0;i<myGUS.ActiveChannels;i++)
			guschan[i]->generateSamples(buf32,len);
	}
	else {
		for(i=0;i<myGUS.ActiveChannels;i++)
			buf32[i] = 0;
	}

	for(i=0;i<len*2;i++) {
		Bit32s sample=((buf32[i] >> 13)*AutoAmp)>>9;
		if (sample>32767) {
			sample=32767;                       
			AutoAmp--;
		} else if (sample<-32768) {
			sample=-32768;
			AutoAmp--;
		}
		buf16[i] = (Bit16s)(sample);
	}
	gus_chan->AddSamples_s16(len,buf16);
	CheckVoiceIrq();
}

// Generate logarithmic to linear volume conversion tables
static void MakeTables(void) {
	int i;
	double out = (double)(1 << 13);
	for (i=4095;i>=0;i--) {
		vol16bit[i]=(Bit16s)out;
		out/=1.002709201;		/* 0.0235 dB Steps */
	}
	/* FIX: DOSBox 0.74 had code here that produced a pantable which
	 *      had nothing to do with actual panning control variables.
	 *      Instead it seemed to generate a 16-element map that started
	 *      at 0, jumped sharply to unity and decayed to 0.
	 *      The unfortunate result was that stock builds of DOSBox
	 *      effectively locked Gravis Ultrasound capable programs
	 *      to monural audio.
	 *
	 *      This fix generates the table properly so that they correspond
	 *      to how much we attenuate the LEFT channel for any given
	 *      4-bit value of the Panning register (you attenuate the
	 *      RIGHT channel by looking at element 0xF - (val&0xF)).
	 *
	 *      Having made this fix I can finally enjoy old DOS demos
	 *      in GUS stereo instead of having everything mushed into
	 *      mono. */
	if (gus_fixed_table) {
		for (i=0;i < 16;i++)
			pantable[i] = pantablePDF[15 - i/*FIXME: Is this backwards?*/] * 2048;
	}
	else {
		for (i=0;i < 8;i++)
			pantable[i] = 0;
		for (i=8;i < 15;i++)
			pantable[i]=(Bit32u)(-128.0*(log((double)(15-i)/7.0)/log(2.0))*(double)(1 << RAMP_FRACT));

		/* if the program cranks the pan register all the way, ensure the
		 * opposite channel is crushed to silence */
		pantable[15] = 1UL << 30UL;
	}
}

class GUS:public Module_base{
private:
	IO_ReadHandleObject ReadHandler[8];
	IO_WriteHandleObject WriteHandler[9];
	AutoexecObject autoexecline[2];
	MixerObject MixerChan;
public:
	GUS(Section* configuration):Module_base(configuration){
		int x;

		if(!IS_EGAVGA_ARCH) return;
		Section_prop * section=static_cast<Section_prop *>(configuration);
		if(!section->Get_bool("gus")) return;
	
		memset(&myGUS,0,sizeof(myGUS));
		memset(GUSRam,0,1024*1024);

		string s_pantable = section->Get_string("gus panning table");
		if (s_pantable == "default" || s_pantable == "" || s_pantable == "accurate")
			gus_fixed_table = true;
		else if (s_pantable == "old")
			gus_fixed_table = false;
		else
			gus_fixed_table = true;

		myGUS.force_master_irq_enable=section->Get_bool("force master irq enable");
		if (myGUS.force_master_irq_enable)
			LOG(LOG_MISC,LOG_DEBUG)("GUS: Master IRQ enable will be forced on as instructed");

		myGUS.rate=section->Get_int("gusrate");

		x = section->Get_int("gusmemsize");
		if (x >= 0) myGUS.memsize = x*1024;
		else myGUS.memsize = 1024*1024;

		if (myGUS.memsize > (1024*1024))
			myGUS.memsize = (1024*1024);

		if ((myGUS.memsize&((256 << 10) - 1)) != 0)
			LOG(LOG_MISC,LOG_WARN)("GUS emulation warning: %uKB onboard is an unusual value. Usually GUS cards have some multiple of 256KB RAM onboard",myGUS.memsize>>10);

		LOG(LOG_MISC,LOG_DEBUG)("GUS emulation: %uKB onboard",myGUS.memsize>>10);

		// FIXME: HUH?? Read the port number and subtract 0x200, then use GUS_BASE
		// in other parts of the code to compare against 0x200 and 0x300? That's confusing. Fix!
		myGUS.portbase = section->Get_hex("gusbase") - 0x200;

		// TODO: so, if the GUS ULTRASND variable actually mentions two DMA and two IRQ channels,
		//       shouldn't we offer the ability to specify them independently? especially when
		//       GUS NMI is completed to the extent that SBOS and MEGA-EM can run within DOSBox?
		int dma_val = section->Get_int("gusdma");
		if ((dma_val<0) || (dma_val>255)) dma_val = 3;	// sensible default

		int irq_val = section->Get_int("gusirq");
		if ((irq_val<0) || (irq_val>255)) irq_val = 5;	// sensible default

		myGUS.dma1 = (Bit8u)dma_val;
		myGUS.dma2 = (Bit8u)dma_val;
		myGUS.irq1 = (Bit8u)irq_val;
		myGUS.irq2 = (Bit8u)irq_val;

		// We'll leave the MIDI interface to the MPU-401 
		// Ditto for the Joystick 
		// GF1 Synthesizer 
		ReadHandler[0].Install(0x302 + GUS_BASE,read_gus,IO_MB);
		WriteHandler[0].Install(0x302 + GUS_BASE,write_gus,IO_MB);
	
		WriteHandler[1].Install(0x303 + GUS_BASE,write_gus,IO_MB);
		ReadHandler[1].Install(0x303 + GUS_BASE,read_gus,IO_MB);
	
		WriteHandler[2].Install(0x304 + GUS_BASE,write_gus,IO_MB|IO_MW);
		ReadHandler[2].Install(0x304 + GUS_BASE,read_gus,IO_MB|IO_MW);
	
		WriteHandler[3].Install(0x305 + GUS_BASE,write_gus,IO_MB);
		ReadHandler[3].Install(0x305 + GUS_BASE,read_gus,IO_MB);
	
		ReadHandler[4].Install(0x206 + GUS_BASE,read_gus,IO_MB);
	
		WriteHandler[4].Install(0x208 + GUS_BASE,write_gus,IO_MB);
		ReadHandler[5].Install(0x208 + GUS_BASE,read_gus,IO_MB);
	
		WriteHandler[5].Install(0x209 + GUS_BASE,write_gus,IO_MB);
	
		WriteHandler[6].Install(0x307 + GUS_BASE,write_gus,IO_MB);
		ReadHandler[6].Install(0x307 + GUS_BASE,read_gus,IO_MB);
	
		// Board Only 
	
		WriteHandler[7].Install(0x200 + GUS_BASE,write_gus,IO_MB);
		ReadHandler[7].Install(0x20A + GUS_BASE,read_gus,IO_MB);
		WriteHandler[8].Install(0x20B + GUS_BASE,write_gus,IO_MB);
	
	//	DmaChannels[myGUS.dma1]->Register_TC_Callback(GUS_DMA_TC_Callback);
	
		MakeTables();
	
		for (Bit8u chan_ct=0; chan_ct<32; chan_ct++) {
			guschan[chan_ct] = new GUSChannels(chan_ct);
		}
		// Register the Mixer CallBack 
		gus_chan=MixerChan.Install(GUS_CallBack,GUS_RATE,"GUS");

		// FIXME: Could we leave the card in reset state until a fake ULTRINIT runs?
		myGUS.gRegData=0x000/*reset*/;
		GUSReset();

		gus_chan->Enable(true);

		int portat = 0x200+GUS_BASE;

		// ULTRASND=Port,DMA1,DMA2,IRQ1,IRQ2
		// [GUS port], [GUS DMA (recording)], [GUS DMA (playback)], [GUS IRQ (playback)], [GUS IRQ (MIDI)]
		ostringstream temp;
		temp << "SET ULTRASND=" << hex << setw(3) << portat << ","
		     << dec << (Bitu)myGUS.dma1 << "," << (Bitu)myGUS.dma2 << ","
		     << (Bitu)myGUS.irq1 << "," << (Bitu)myGUS.irq2 << ends;
		// Create autoexec.bat lines
		autoexecline[0].Install(temp.str());
		autoexecline[1].Install(std::string("SET ULTRADIR=") + section->Get_string("ultradir"));
	}

	void DOS_Shutdown() { /* very likely, we're booting into a guest OS where our environment variable has no meaning anymore */
		autoexecline[0].Uninstall();
		autoexecline[1].Uninstall();
	}

	~GUS() {
#if 0 // FIXME
		if(!IS_EGAVGA_ARCH) return;
	
		myGUS.gRegData=0x1;
		GUSReset();
		myGUS.gRegData=0x0;
	
		for(Bitu i=0;i<32;i++) {
			delete guschan[i];
		}

		memset(&myGUS,0,sizeof(myGUS));
		memset(GUSRam,0,1024*1024);
#endif
	}
};

static GUS* test = NULL;

void GUS_DOS_Shutdown() {
	if (test != NULL) test->DOS_Shutdown();
}

void GUS_ShutDown(Section* /*sec*/) {
	if (test != NULL) {
		delete test;	
		test = NULL;
	}
}

void GUS_OnReset(Section *sec) {
	if (test == NULL) {
		LOG(LOG_MISC,LOG_DEBUG)("Allocating GUS emulation");
		test = new GUS(control->GetSection("gus"));
	}
}

void GUS_Init() {
	LOG(LOG_MISC,LOG_DEBUG)("Initializing Gravis Ultrasound emulation");

	AddExitFunction(AddExitFunctionFuncPair(GUS_ShutDown),true);
	AddVMEventFunction(VM_EVENT_RESET,AddVMEventFunctionFuncPair(GUS_OnReset));
}

