#include "Board.h"
#include "tmc/ic/TMC2209/TMC2209.h"
#include "../tmc/StepDir.h"

#undef  TMC2209_MAX_VELOCITY
#define TMC2209_MAX_VELOCITY  STEPDIR_MAX_VELOCITY

#define ERRORS_VM        (1<<0)
#define ERRORS_VM_UNDER  (1<<1)
#define ERRORS_VM_OVER   (1<<2)

#define VM_MIN  50   // VM[V/10] min
#define VM_MAX  390  // VM[V/10] max

#define MOTORS 1

#define TIMEOUT_VALUE 10 // 10 ms

static uint32_t right(uint8_t motor, int32_t velocity);
static uint32_t left(uint8_t motor, int32_t velocity);
static uint32_t rotate(uint8_t motor, int32_t velocity);
static uint32_t stop(uint8_t motor);
static uint32_t moveTo(uint8_t motor, int32_t position);
static uint32_t moveBy(uint8_t motor, int32_t *ticks);
static uint32_t GAP(uint8_t type, uint8_t motor, int32_t *value);
static uint32_t SAP(uint8_t type, uint8_t motor, int32_t value);

static uint32_t setPins(uint32_t pins);

static void checkErrors (uint32_t tick);
static void deInit(void);
static uint32_t userFunction(uint8_t type, uint8_t motor, int32_t *value);

static void periodicJob(uint32_t tick);
static uint8_t reset(void);
static void enableDriver(DriverState state);

static UART_Config *TMC2209_UARTChannel;
static TMC2209TypeDef TMC2209;
static ConfigurationTypeDef *TMC2209_config;

static uint32_t pin_states = 0;
static bool pin_configurator = false;

// Helper macro - index is always 1 here (channel 1 <-> index 0, channel 2 <-> index 1)
#define TMC2209_CRC(data, length) tmc_CRC8(data, length, 1)

typedef struct
{
	IOPinTypeDef  *ENN;
	IOPinTypeDef  *SPREAD;
	IOPinTypeDef  *STEP;
	IOPinTypeDef  *DIR;
	IOPinTypeDef  *MS1_AD0;
	IOPinTypeDef  *MS2_AD1;
	IOPinTypeDef  *DIAG;
	IOPinTypeDef  *INDEX;
	IOPinTypeDef  *UC_PWM;
	IOPinTypeDef  *STDBY;
} PinsTypeDef;

static PinsTypeDef Pins;

static uint8_t restore(void);

void tmc2209_writeRegister(uint8_t motor, uint8_t address, int32_t value)
{
	UNUSED(motor);
	UART_writeInt(TMC2209_UARTChannel, tmc2209_get_slave(&TMC2209), address, value);
	TMC2209.config->shadowRegister[TMC_ADDRESS(address)] = value;
}

void tmc2209_readRegister(uint8_t motor, uint8_t address, int32_t *value)
{
	UNUSED(motor);
	if(pin_configurator && address == 0) // Detect old Rhino setPins datagram
		*value = setPins(*value);
	else {
		if(TMC_IS_READABLE(TMC2209.registerAccess[TMC_ADDRESS(address)]))
			UART_readInt(TMC2209_UARTChannel, tmc2209_get_slave(&TMC2209), address, value);
		else
			*value = TMC2209.config->shadowRegister[TMC_ADDRESS(address)];
	}
}

static uint32_t rotate(uint8_t motor, int32_t velocity)
{
	if(motor >= MOTORS)
		return TMC_ERROR_MOTOR;

	StepDir_rotate(motor, velocity);

	return TMC_ERROR_NONE;
}

static uint32_t right(uint8_t motor, int32_t velocity)
{
	return rotate(motor, velocity);
}

static uint32_t left(uint8_t motor, int32_t velocity)
{
	return rotate(motor, -velocity);
}

static uint32_t stop(uint8_t motor)
{
	return rotate(motor, 0);
}

static uint32_t moveTo(uint8_t motor, int32_t position)
{
	if(motor >= MOTORS)
		return TMC_ERROR_MOTOR;

	StepDir_moveTo(motor, position);

	return TMC_ERROR_NONE;
}

static uint32_t moveBy(uint8_t motor, int32_t *ticks)
{
	if(motor >= MOTORS)
		return TMC_ERROR_MOTOR;

	// determine actual position and add numbers of ticks to move
	*ticks += StepDir_getActualPosition(motor);

	return moveTo(motor, *ticks);
}

static uint32_t handleParameter(uint8_t readWrite, uint8_t motor, uint8_t type, int32_t *value)
{
	uint32_t errors = TMC_ERROR_NONE;


	if(motor >= MOTORS)
		return TMC_ERROR_MOTOR;

	switch(type)
	{
	case 0:
		// Target position
		if(readWrite == READ) {
			*value = StepDir_getTargetPosition(motor);
		} else if(readWrite == WRITE) {
			StepDir_moveTo(motor, *value);
		}
		break;
	case 1:
		// Actual position
		if(readWrite == READ) {
			*value = StepDir_getActualPosition(motor);
		} else if(readWrite == WRITE) {
			StepDir_setActualPosition(motor, *value);
		}
		break;
	case 2:
		// Target speed
		if(readWrite == READ) {
			*value = StepDir_getTargetVelocity(motor);
		} else if(readWrite == WRITE) {
			StepDir_rotate(motor, *value);
		}
		break;
	case 3:
		// Actual speed
		if(readWrite == READ) {
			*value = StepDir_getActualVelocity(motor);
		} else if(readWrite == WRITE) {
			errors |= TMC_ERROR_TYPE;
		}
		break;
	case 4:
		// Maximum speed
		if(readWrite == READ) {
			*value = StepDir_getVelocityMax(motor);
		} else if(readWrite == WRITE) {
			StepDir_setVelocityMax(motor, abs(*value));
		}
		break;
	case 5:
		// Maximum acceleration
		if(readWrite == READ) {
			*value = StepDir_getAcceleration(motor);
		} else if(readWrite == WRITE) {
			StepDir_setAcceleration(motor, *value);
		}
		break;
	case 6:
		// UART slave address
		if(readWrite == READ) {
			*value = tmc2209_get_slave(&TMC2209);
		} else if(readWrite == WRITE) {
			tmc2209_set_slave(&TMC2209, *value);
		}
		break;
	default:
		errors |= TMC_ERROR_TYPE;
		break;
	}

	return errors;
}

static uint32_t SAP(uint8_t type, uint8_t motor, int32_t value)
{
	return handleParameter(WRITE, motor, type, &value);
}

static uint32_t GAP(uint8_t type, uint8_t motor, int32_t *value)
{
	return handleParameter(READ, motor, type, value);
}

static uint32_t setPins(uint32_t pins)
{
	uint8_t state = 0;
	IOPinTypeDef *pin = Pins.ENN;
	for(uint8_t i = 0; pins; i++) {
		switch(i) {
		case 0:
			pin = Pins.ENN;
			break;
		case 1:
			pin = Pins.SPREAD;
			break;
		case 2:
			pin = Pins.MS1_AD0;
			break;
		case 3:
			pin = Pins.MS2_AD1;
			break;
		case 4:
			pin = Pins.UC_PWM;
			break;
		case 5:
			pin = Pins.STDBY;
			break;
		default:
			goto error;
		}
		state = pins & 0x03;
		HAL.IOs->config->toOutput(pin);
		switch(state) {
		case 0b01: // VCC_IO
			HAL.IOs->config->setHigh(pin);
			break;
		case 0b10: // open
			HAL.IOs->config->reset(pin);
			break;
		case 0b00: // GND
			HAL.IOs->config->setLow(pin);
			break;
		case 0b11:
			goto shift;
			break;
		}
		pin_states = FIELD_SET(pin_states, 0x03 << (i << 1), (i << 1), state);
		shift: pins >>= 2;
	}
	error: return pin_states;
}

static void checkErrors(uint32_t tick)
{
	UNUSED(tick);
	Evalboards.ch2.errors = 0;
}

static uint32_t userFunction(uint8_t type, uint8_t motor, int32_t *value)
{
	uint32_t errors = 0;
	uint8_t state;
	IOPinTypeDef *pin;

	switch(type)
	{
	case 0:  // Read StepDir status bits
		*value = StepDir_getStatus(motor);
		break;
	case 1:
		tmc2209_set_slave(&TMC2209, (*value) & 0xFF);
		break;
	case 2:
		*value = tmc2209_get_slave(&TMC2209);
		break;
	case 3:
		pin_configurator = (*value == 1);
		break;
	case 4:
		Timer.setDuty(TIMER_CHANNEL_3, (uint32_t) ((uint32_t)(*value) * (uint32_t)TIMER_MAX) / (uint32_t)100);
		break;
	case 5: // Set pin state
		state = (*value) & 0x03;
		pin = Pins.ENN;
		switch(motor) {
		case 0:
			pin = Pins.ENN;
			break;
		case 1:
			pin = Pins.SPREAD;
			break;
		case 2:
			pin = Pins.MS1_AD0;
			break;
		case 3:
			pin = Pins.MS2_AD1;
			break;
		case 4:
			pin = Pins.UC_PWM;
			break;
		case 5:
			pin = Pins.STDBY;
			break;
		}
		HAL.IOs->config->setToState(pin, state);
		break;
	case 6: // Get pin state
		pin = Pins.ENN;
		switch(motor) {
		case 0:
			pin = Pins.ENN;
			break;
		case 1:
			pin = Pins.SPREAD;
			break;
		case 2:
			pin = Pins.MS1_AD0;
			break;
		case 3:
			pin = Pins.MS2_AD1;
			break;
		case 4:
			pin = Pins.UC_PWM;
			break;
		case 5:
			pin = Pins.STDBY;
			break;
		}
		*value = (uint32_t) HAL.IOs->config->getState(pin);
		break;
	default:
		errors |= TMC_ERROR_TYPE;
		break;
	}

	return errors;
}

static void deInit(void)
{
	enableDriver(DRIVER_DISABLE);
	HAL.IOs->config->reset(Pins.ENN);
	HAL.IOs->config->reset(Pins.SPREAD);
	HAL.IOs->config->reset(Pins.STEP);
	HAL.IOs->config->reset(Pins.DIR);
	HAL.IOs->config->reset(Pins.MS1_AD0);
	HAL.IOs->config->reset(Pins.MS2_AD1);
	HAL.IOs->config->reset(Pins.DIAG);
	HAL.IOs->config->reset(Pins.INDEX);
	HAL.IOs->config->reset(Pins.STDBY);
	HAL.IOs->config->reset(Pins.UC_PWM);

	StepDir_deInit();
	Timer.deInit();
}

static uint8_t reset()
{
	StepDir_init();
	StepDir_setPins(0, Pins.STEP, Pins.DIR, NULL);

	return tmc2209_reset(&TMC2209);
}

static uint8_t restore()
{
	return tmc2209_restore(&TMC2209);
}

static void enableDriver(DriverState state)
{
	if(state == DRIVER_USE_GLOBAL_ENABLE)
		state = Evalboards.driverEnable;

	if(state == DRIVER_DISABLE)
		HAL.IOs->config->setHigh(Pins.ENN);
	else if((state == DRIVER_ENABLE) && (Evalboards.driverEnable == DRIVER_ENABLE))
		HAL.IOs->config->setLow(Pins.ENN);
}

static void periodicJob(uint32_t tick)
{
	for(int motor = 0; motor < MOTORS; motor++)
	{
		tmc2209_periodicJob(&TMC2209, tick);
		StepDir_periodicJob(motor);
	}
}

void TMC2209_init(void)
{
	tmc_fillCRC8Table(0x07, true, 1);

	Pins.ENN      = &HAL.IOs->pins->DIO0;
	Pins.SPREAD   = &HAL.IOs->pins->DIO8;
	Pins.STEP     = &HAL.IOs->pins->DIO6;
	Pins.DIR      = &HAL.IOs->pins->DIO7;
	Pins.MS1_AD0  = &HAL.IOs->pins->DIO3;
	Pins.MS2_AD1  = &HAL.IOs->pins->DIO4;
	Pins.DIAG     = &HAL.IOs->pins->DIO1;
	Pins.INDEX    = &HAL.IOs->pins->DIO2;
	Pins.UC_PWM   = &HAL.IOs->pins->DIO9;
	Pins.STDBY    = &HAL.IOs->pins->DIO0;

	HAL.IOs->config->toOutput(Pins.ENN);
	HAL.IOs->config->toOutput(Pins.SPREAD);
	HAL.IOs->config->toOutput(Pins.STEP);
	HAL.IOs->config->toOutput(Pins.DIR);
	HAL.IOs->config->toOutput(Pins.MS1_AD0);
	HAL.IOs->config->toOutput(Pins.MS2_AD1);
	HAL.IOs->config->toInput(Pins.DIAG);
	HAL.IOs->config->toInput(Pins.INDEX);

	HAL.IOs->config->setLow(Pins.MS1_AD0);
	HAL.IOs->config->setLow(Pins.MS2_AD1);
	HAL.IOs->config->setToState(Pins.MS1_AD0, IOS_HIGH);

	TMC2209_UARTChannel = HAL.UART;
	TMC2209_UARTChannel->pinout = UART_PINS_2;
	TMC2209_UARTChannel->rxtx.init();

	TMC2209_config = Evalboards.ch2.config;

	Evalboards.ch2.config->reset        = reset;
	Evalboards.ch2.config->restore      = restore;
	Evalboards.ch2.config->state        = CONFIG_RESET;
	Evalboards.ch2.config->configIndex  = 0;

	Evalboards.ch2.rotate               = rotate;
	Evalboards.ch2.right                = right;
	Evalboards.ch2.left                 = left;
	Evalboards.ch2.stop                 = stop;
	Evalboards.ch2.GAP                  = GAP;
	Evalboards.ch2.SAP                  = SAP;
	Evalboards.ch2.moveTo               = moveTo;
	Evalboards.ch2.moveBy               = moveBy;
	Evalboards.ch2.writeRegister        = tmc2209_writeRegister;
	Evalboards.ch2.readRegister         = tmc2209_readRegister;
	Evalboards.ch2.userFunction         = userFunction;
	Evalboards.ch2.enableDriver         = enableDriver;
	Evalboards.ch2.checkErrors          = checkErrors;
	Evalboards.ch2.numberOfMotors       = MOTORS;
	Evalboards.ch2.VMMin                = VM_MIN;
	Evalboards.ch2.VMMax                = VM_MAX;
	Evalboards.ch2.deInit               = deInit;
	Evalboards.ch2.periodicJob          = periodicJob;

	tmc2209_init(&TMC2209, 0, TMC2209_config, &tmc2209_defaultRegisterResetState[0]);

	StepDir_init();
	StepDir_setPins(0, Pins.STEP, Pins.DIR, NULL);
	StepDir_setVelocityMax(0, 51200);
	StepDir_setAcceleration(0, 51200);

#if defined(Startrampe)
	Pins.UC_PWM->configuration.GPIO_Mode = GPIO_Mode_AF;
	GPIO_PinAFConfig(Pins.UC_PWM->port, Pins.UC_PWM->bit, GPIO_AF_TIM1);
#elif defined(Landungsbruecke)
	HAL.IOs->config->toOutput(Pins.UC_PWM);
	Pins.UC_PWM->configuration.GPIO_Mode = GPIO_Mode_AF4;
#endif

	HAL.IOs->config->set(Pins.UC_PWM);
	Timer.init();
	Timer.setDuty(TIMER_CHANNEL_3, 0);

	enableDriver(DRIVER_ENABLE);
};