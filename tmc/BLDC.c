/*
 * BLDC.c
 *
 *  Created on: 14 Apr 2020
 *      Author: LH
 */

#include "BLDC.h"
#include "hal/HAL.h"

// FTM0_OUTMASK: 0-normal 1-inactive
#define PWM_PHASE_U_ENABLED		0x00
#define PWM_PHASE_U_DISABLED	0x03
#define PWM_PHASE_V_ENABLED		0x00
#define PWM_PHASE_V_DISABLED 	0x30
#define PWM_PHASE_W_ENABLED		0x00
#define PWM_PHASE_W_DISABLED	0xC0

#define PWM_FREQ 		  20000                  // in Hz
#define PWM_PERIOD 		  (48000000 / PWM_FREQ)  // 48MHz/2*20kHz = 2500

int16_t  targetPWM        = 0;
uint16_t openloopStepTime = 200;
BLDCMode commutationMode  = BLDC_OPENLOOP;
uint8_t  pwmEnabled       = 0;
uint8_t  bbmTime          = 50;

int targetAngle         = 0;
int hallAngle           = 0;

volatile int32_t adcSamples[4] = { 0 };
uint8_t adcSampleIndex = 0;

int32_t adcOffset = 0;
uint32_t sampleCount = 0;

#define ADC_SAMPLES 100

// todo: Calculate this and describe the calculation instead of measuring it
#define CURRENT_SCALING_FACTOR 10712

typedef enum {
	HALL_INVALID_0 = 0,

	HALL_001 = 1,
	HALL_010 = 2,
	HALL_011 = 3,
	HALL_100 = 4,
	HALL_101 = 5,
	HALL_110 = 6,

	HALL_INVALID_1 = 7,
} HallStates;

static int16_t hallStateToAngle(HallStates hallState);
static HallStates inputToHallState(uint8_t in_0, uint8_t in_1, uint8_t in_2);

// Hall parameters
uint8_t hallOrder = 0;
uint8_t hallInvert = 0;

enum {
	ADC_INIT,
	ADC_READY
} adcState = ADC_INIT;

typedef struct
{
	IOPinTypeDef  *HALL_U;
	IOPinTypeDef  *HALL_V;
	IOPinTypeDef  *HALL_W;
	IOPinTypeDef  *PWM_UL;
	IOPinTypeDef  *PWM_UH;
	IOPinTypeDef  *PWM_VL;
	IOPinTypeDef  *PWM_VH;
	IOPinTypeDef  *PWM_WL;
	IOPinTypeDef  *PWM_WH;
} PinsTypeDef;

static PinsTypeDef Pins;

static HallStates inputToHallState(uint8_t in_0, uint8_t in_1, uint8_t in_2)
{
	uint8_t tmp;
	HallStates retVal = HALL_INVALID_0;

	if (hallInvert)
	{
		// Swap in_1 and in_2
		tmp = in_1;
		in_1 = in_2;
		in_2 = tmp;
	}

	switch(hallOrder)
	{
	case 0: // U/V/W
		retVal = in_0 << 0
		       | in_1 << 1
		       | in_2 << 2;
		break;
	case 1: // V/W/U
		retVal = in_0 << 1
		       | in_1 << 2
		       | in_2 << 0;
		break;
	case 2: // W/U/V
		retVal = in_0 << 2
		       | in_1 << 0
		       | in_2 << 1;
		break;
	}

	return retVal;
}

static int16_t hallStateToAngle(HallStates hallState)
{
	switch (hallState)
	{
	case HALL_001:
		return 0;
		break;
	case HALL_011:
		return 60;
		break;
	case HALL_010:
		return 120;
		break;
	case HALL_110:
		return 180;
		break;
	case HALL_100:
		return 240;
		break;
	case HALL_101:
		return 300;
		break;
	default:
		break;
	}

	return 0;
}

void BLDC_init(IOPinTypeDef *hallU, IOPinTypeDef *hallV, IOPinTypeDef *hallW)
{
	Pins.PWM_UL       = &HAL.IOs->pins->DIO6;
	Pins.PWM_UH       = &HAL.IOs->pins->DIO7;
	Pins.PWM_VL       = &HAL.IOs->pins->DIO8;
	Pins.PWM_VH       = &HAL.IOs->pins->DIO9;
	Pins.PWM_WL       = &HAL.IOs->pins->DIO10;
	Pins.PWM_WH       = &HAL.IOs->pins->DIO11;

	Pins.HALL_U       = hallU;
	Pins.HALL_V       = hallV;
	Pins.HALL_W       = hallW;

	HAL.IOs->config->toInput(Pins.HALL_U);
	HAL.IOs->config->toInput(Pins.HALL_V);
	HAL.IOs->config->toInput(Pins.HALL_W);

	// --- PDB ---
	// Enable clock for programmable delay block (PDB)
	SIM_SCGC6 |= SIM_SCGC6_PDB_MASK;

	PDB0_MOD = PWM_PERIOD - 10;

	PDB0_CH1C1 = PDB_C1_TOS(0x01)
	           | PDB_C1_EN(0x01);

	PDB0_CH1DLY0 = 3;

	PDB0_SC = PDB_SC_LDMOD(3)
			| PDB_SC_PDBEN_MASK    // enable PDB
			| PDB_SC_TRGSEL(0x08)  // 0x08 = FTM0
			//| PDB_SC_TRGSEL(0x0F) // 0x0F = software
			| PDB_SC_PDBIE_MASK
			| PDB_SC_LDOK_MASK;

	// --- ADC ---
	// Enable clock for ADC1
	SIM_SCGC3 |= SIM_SCGC3_ADC1_MASK;

	// Input DADP1
	ADC1_SC1A = ADC_SC1_ADCH(0) | ADC_SC1_AIEN_MASK;

	// Single-ended 16 bit conversion, ADCK = Bus Clock/2
	ADC1_CFG1 = ADC_CFG1_MODE(0x03) | ADC_CFG1_ADICLK(1);

	ADC1_CFG2 = ADC_CFG2_ADHSC_MASK
	          //| ADC_CFG2_ADLSTS(3) // Should be useless
			  ;

	// Hardware trigger
	ADC1_SC2 = ADC_SC2_ADTRG_MASK;

	ADC1_SC3 = ADC_SC3_CAL_MASK;
	while(ADC1_SC3 & ADC_SC3_CAL_MASK);

    // set ADC1 interrupt handler
	enable_irq(INT_ADC1-16);

	// --- FTM ---
	// Deinitialize the timer, as we're going to take FTM0 over
	Timer.deInit();

	// enable clock for FTM0
	SIM_SCGC6 |= SIM_SCGC6_FTM0_MASK;

	// disable write protection
	FTM0_MODE |= FTM_MODE_WPDIS_MASK;

	FTM0_CONF |= FTM_CONF_BDMMODE(3); // let counter run in BDM mode

	// FAULTM = 11 - Fault control is enabled for all channels (automatic fault clearing)
	// FTMEN  = 1 - all registers are available for use with no restrictions
	FTM0_MODE |= FTM_MODE_FAULTM_MASK | FTM_MODE_FTMEN_MASK;

	// setting for Center Aligned PWM in Combine Mode
	FTM0_MOD = PWM_PERIOD-1;
	FTM0_CNTIN = 0x00;
	FTM0_SYNCONF |= FTM_SYNCONF_SYNCMODE_MASK; // set enhanced PWM sync mode

	FTM0_SYNC |= FTM_SYNC_CNTMAX_MASK; // use FTM0_MOD as max loading point
	FTM0_SYNC |= FTM_SYNC_SWSYNC_MASK; // use software trigger as pwm synchronization trigger

	// disable all channels outputs
	FTM0_OUTMASK = FTM_OUTMASK_CH7OM_MASK | FTM_OUTMASK_CH6OM_MASK |
				   FTM_OUTMASK_CH5OM_MASK | FTM_OUTMASK_CH4OM_MASK |
				   FTM_OUTMASK_CH1OM_MASK | FTM_OUTMASK_CH0OM_MASK;

	// COMBINE = 1 - combine mode set, COMP = 1 - complementary PWM set,
	// DTEN = 1 - deadtime enabled, SYNCEN = 1 - PWM update synchronization enabled,
	// FAULTEN = 1 - fault control enabled
	FTM0_COMBINE = FTM_COMBINE_SYNCEN0_MASK | FTM_COMBINE_DTEN0_MASK
			| FTM_COMBINE_COMP0_MASK | FTM_COMBINE_COMBINE0_MASK
			| FTM_COMBINE_SYNCEN2_MASK | FTM_COMBINE_DTEN2_MASK
			| FTM_COMBINE_COMP2_MASK | FTM_COMBINE_COMBINE2_MASK
			| FTM_COMBINE_SYNCEN3_MASK | FTM_COMBINE_DTEN3_MASK
			| FTM_COMBINE_COMP3_MASK | FTM_COMBINE_COMBINE3_MASK
			| FTM_COMBINE_FAULTEN0_MASK | FTM_COMBINE_FAULTEN2_MASK | FTM_COMBINE_FAULTEN3_MASK;

	// set dead time prescaler and dead time
	FTM0_DEADTIME = FTM_DEADTIME_DTVAL(bbmTime);

	/* Initial setting of value registers */
	FTM0_C0V = 0;
	FTM0_C1V = 0;
	FTM0_C4V = 0;
	FTM0_C5V = 0;
	FTM0_C6V = 0;
	FTM0_C7V = 0;

	// set channel mode to generate positive PWM
	FTM0_C0SC |= FTM_CnSC_ELSB_MASK;
	FTM0_C1SC |= FTM_CnSC_ELSB_MASK;
	FTM0_C4SC |= FTM_CnSC_ELSB_MASK;
	FTM0_C5SC |= FTM_CnSC_ELSB_MASK;
	FTM0_C6SC |= FTM_CnSC_ELSB_MASK;
	FTM0_C7SC |= FTM_CnSC_ELSB_MASK;

	// enable loading of the MOD, CNTIN, and CV registers with the values of their write buffers
	FTM0_PWMLOAD = FTM_PWMLOAD_LDOK_MASK;

	// enable the generation of the trigger when the FTM counter is equal to the CNTIN register
	FTM0_EXTTRIG |= FTM_EXTTRIG_INITTRIGEN_MASK;
	FTM0_MODE |= FTM_MODE_INIT_MASK;

	// set system clock as source for FTM0 (CLKS[1:0] = 01)
	FTM0_SC |= FTM_SC_CLKS(1);

    // enable all PWM output channels of FTM0
    FTM0_OUTMASK = 0;

    // enable FTM0 interrupt
    FTM0_SC |= (uint32_t)(FTM_SC_TOIE_MASK);

    // set FTM0 interrupt handler
	enable_irq(INT_FTM0-16);
}

void ADC1_IRQHandler()
{
	int tmp = ADC1_RA;

	switch(adcState)
	{
	case ADC_INIT:
		if (sampleCount < ADC_SAMPLES)
		{
			// Store a calibration sample
			adcOffset += tmp;

			sampleCount++;
		}
		else
		{
			// Finished collection of calibration samples
			// Calculate offset
			adcOffset = adcOffset / ADC_SAMPLES;

			adcState = ADC_READY;
		}
		break;
	case ADC_READY:
		adcSamples[adcSampleIndex] = (tmp - adcOffset) * CURRENT_SCALING_FACTOR / 65536;
		adcSampleIndex = (adcSampleIndex + 1) % ARRAY_SIZE(adcSamples);

		break;
	}
}

void FTM0_IRQHandler()
{
	// clear timer overflow flag
	FTM0_SC &= ~FTM_SC_TOF_MASK;

	static int commutationCounter = 0;

	// Measure the hall sensor
	HallStates actualHallState = inputToHallState(HAL.IOs->config->isHigh(Pins.HALL_U), HAL.IOs->config->isHigh(Pins.HALL_V), HAL.IOs->config->isHigh(Pins.HALL_W));

	hallAngle = hallStateToAngle(actualHallState);

	if (commutationMode == BLDC_OPENLOOP)
	{
		// open loop mode
		if (openloopStepTime)
		{
			if (++commutationCounter >= openloopStepTime)
			{
				if (targetPWM > 0)
					targetAngle = (targetAngle + 60) % 360;
				else if (targetPWM < 0)
					targetAngle = (targetAngle - 60 + 360) % 360;

				commutationCounter = 0;
			}
		}
	}
	else if (commutationMode == BLDC_HALL)
	{
		if (targetPWM > 0)
		{
			// The +30 are to compensate hall getting rounded to the nearest 60° step
			targetAngle = ((hallAngle + 30) + 90) % 360;
		}
		else if (targetPWM < 0)
		{
			// The +30 are to compensate hall getting rounded to the nearest 60° step
			// The +360 are to prevent a negative operand for the modulo.
			targetAngle = ((hallAngle + 30) - 90 + 360) % 360;
		}
		else
		{
			targetAngle = hallAngle;
		}
	}

	// update commutation step
	int16_t duty = ((int32_t)targetPWM * ((int32_t)PWM_PERIOD-1))/(int32_t)s16_MAX;

	if (duty < 0)
		duty = -duty;

	switch (targetAngle % 360)
	{
	case 0:
		FTM0_OUTMASK = PWM_PHASE_U_DISABLED | // off
		               PWM_PHASE_V_ENABLED  | // high with pwm
		               PWM_PHASE_W_ENABLED;   // low

		FTM0_C1V = 0;
		FTM0_C5V = duty;
		FTM0_C7V = 0;
		break;
	case 60:
		FTM0_OUTMASK = PWM_PHASE_U_ENABLED  | // low
		               PWM_PHASE_V_ENABLED  | // high with pwm
		               PWM_PHASE_W_DISABLED;  // off

		FTM0_C1V = 0;
		FTM0_C5V = duty;
		FTM0_C7V = 0;
		break;
	case 120:
		FTM0_OUTMASK = PWM_PHASE_U_ENABLED  | // low
		               PWM_PHASE_V_DISABLED | // off
		               PWM_PHASE_W_ENABLED;   // high with pwm

		FTM0_C1V = 0;
		FTM0_C5V = 0;
		FTM0_C7V = duty;
		break;
	case 180:
		FTM0_OUTMASK = PWM_PHASE_U_DISABLED | // off
		               PWM_PHASE_V_ENABLED  | // low
		               PWM_PHASE_W_ENABLED;   // high with pwm

		FTM0_C1V = 0;
		FTM0_C5V = 0;
		FTM0_C7V = duty;
		break;
	case 240:
		FTM0_OUTMASK = PWM_PHASE_U_ENABLED  | // high with pwm
		               PWM_PHASE_V_ENABLED  | // low
		               PWM_PHASE_W_DISABLED;  // off

		FTM0_C1V = duty;
		FTM0_C5V = 0;
		FTM0_C7V = 0;
		break;
	case 300:
		FTM0_OUTMASK = PWM_PHASE_U_ENABLED  | // high with pwm
		               PWM_PHASE_V_DISABLED | // off
		               PWM_PHASE_W_ENABLED;   // low

		FTM0_C1V = duty;
		FTM0_C5V = 0;
		FTM0_C7V = 0;
		break;
	default:
		FTM0_OUTMASK = PWM_PHASE_U_DISABLED | PWM_PHASE_V_DISABLED | PWM_PHASE_W_DISABLED;
		break;
	}

	// Update PDB timing
	PDB0_CH1DLY0 = duty/2;

	// Update PDB and FTM registers
	PDB0_SC |= PDB_SC_LDOK_MASK;
	FTM0_SYNC |= FTM_SYNC_REINIT_MASK;

	// LOAD Enable: enables the loading of the MOD, CNTIN, and
	// CV registers with the values of their write buffers
	FTM0_PWMLOAD = FTM_PWMLOAD_LDOK_MASK;
}

void BLDC_enablePWM(uint8_t enable)
{
	if (enable)
	{
		Pins.PWM_UH->configuration.GPIO_Mode  = GPIO_Mode_AF4;
		Pins.PWM_UH->configuration.GPIO_OType = GPIO_OType_PP;
		Pins.PWM_UH->configuration.GPIO_PuPd  = GPIO_PuPd_NOPULL;
		HAL.IOs->config->set(Pins.PWM_UH);

		HAL.IOs->config->copy(&Pins.PWM_UH->configuration, Pins.PWM_UL);
		HAL.IOs->config->copy(&Pins.PWM_UH->configuration, Pins.PWM_VH);
		HAL.IOs->config->copy(&Pins.PWM_UH->configuration, Pins.PWM_VL);
		HAL.IOs->config->copy(&Pins.PWM_UH->configuration, Pins.PWM_WH);
		HAL.IOs->config->copy(&Pins.PWM_UH->configuration, Pins.PWM_WL);

		pwmEnabled = 1;
	}
	else
	{
		HAL.IOs->config->reset(Pins.PWM_UH);
		HAL.IOs->config->reset(Pins.PWM_UL);
		HAL.IOs->config->reset(Pins.PWM_VH);
		HAL.IOs->config->reset(Pins.PWM_VL);
		HAL.IOs->config->reset(Pins.PWM_WH);
		HAL.IOs->config->reset(Pins.PWM_WL);

		pwmEnabled = 0;
	}
}

uint8_t BLDC_isPWMenabled()
{
	return pwmEnabled;
}

void BLDC_setTargetPWM(int16_t pwm)
{
	targetPWM = pwm;
}

int16_t BLDC_getTargetPWM()
{
	return targetPWM;
}

int BLDC_getMeasuredCurrent()
{
	int32_t sum = 0;
	for (uint8_t i = 0; i < ARRAY_SIZE(adcSamples); i++)
	{
		sum += adcSamples[i];
	}

	return sum / (int32_t) ARRAY_SIZE(adcSamples);
}

void BLDC_setCommutationMode(BLDCMode mode)
{
	commutationMode = mode;
}

BLDCMode BLDC_getCommutationMode()
{
	return commutationMode;
}

void BLDC_setOpenloopStepTime(uint16_t stepTime)
{
	openloopStepTime = stepTime;
}

uint16_t BLDC_getOpenloopStepTime()
{
	return openloopStepTime;
}

int BLDC_getTargetAngle()
{
	return targetAngle;
}

int BLDC_getHallAngle()
{
	return hallAngle;
}

void BLDC_setHallOrder(uint8_t order)
{
	if (order < 3)
	{
		hallOrder = order;
	}
}

uint8_t BLDC_getHallOrder()
{
	return hallOrder;
}

void BLDC_setHallInvert(uint8_t invert)
{
	hallInvert = (invert)? 1:0;
}

uint8_t BLDC_getHallInvert()
{
	return hallInvert;
}

void BLDC_setBBMTime(uint8_t time)
{
	// Clip to the maximum value instead of overflowing
	bbmTime = MIN(time, 63);

	// set dead time prescaler and dead time
	FTM0_DEADTIME = FTM_DEADTIME_DTVAL(bbmTime);
}

uint8_t BLDC_getBBMTime()
{
	return bbmTime;
}
