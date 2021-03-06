/**
 * @file flight_controller.c
 *
 * @brief Handles moving between flight states and scheduling of critical tasks.
 */

#include <stdbool.h>
#include <stdint.h>

#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "driverlib/interrupt.h"
#include "driverlib/sysctl.h"
#include "driverlib/timer.h"
#include "utils/scheduler.h"

#include "buttons.h"
#include "flight_controller.h"
#include "height.h"
#include "height_controller.h"
#include "pwm.h"
#include "switch.h"
#include "yaw.h"
#include "yaw_controller.h"

/**
 * Forward declarations.
 * @{
 */
void TimerInit(void);
void TimerHandler(void);
void UpdateError(void);
void ResetError(void);
bool HasReachedTargetYaw(void);
bool HasReachedTargetHeight(void);
/** @} */

/*
 * Timer definitions.
 */
#define TIMER_PERIPH			SYSCTL_PERIPH_TIMER0
#define TIMER_BASE				TIMER0_BASE
#define TIMER_CONFIG			TIMER_CFG_PERIODIC
#define TIMER_TIMER				TIMER_A
#define TIMER_TIMEOUT			TIMER_TIMA_TIMEOUT
#define TIMER_INT				INT_TIMER0A

/*
 * Rate of descent (ms per decrement of duty cycle)
 */
#define RATE_OF_DESCENT			    35

/*
 * Acceptable tolerance for yaw error (rotation unit defined in yaw.h)
 */
#define YAW_SAMPLE_TOLERANCE        2

/*
 * Acceptable tolerance for height error (%)
 */
#define HEIGHT_SAMPLE_TOLERANCE     1

/*
 * Number of samples to summate error over.
 */
#define NUM_ERROR_SAMPLES           5

static const uint8_t height_inc = 10;
static const uint8_t height_min = 0;
static const uint8_t height_max = 100;
static const uint8_t yaw_inc = 15;

/*
 * Tolerance to ascertain if yaw has reached target yaw.
 */
static const uint16_t yaw_tolerance = YAW_SAMPLE_TOLERANCE * NUM_ERROR_SAMPLES;
static uint16_t yaw_error_buf[NUM_ERROR_SAMPLES];

/*
 * Tolerance to ascertain if height has reached target height.
 */
static const uint16_t height_tolerance = HEIGHT_SAMPLE_TOLERANCE
        * NUM_ERROR_SAMPLES;
static uint16_t height_error_buf[NUM_ERROR_SAMPLES];

static const char* flight_mode[] = { "Landed", "Init", "Flying", "Landing" };
static enum {
    LANDED, INIT, FLYING, LANDING
} flight_state = LANDED;

void TimerHandler(void) {
    TimerIntClear(TIMER_BASE, TIMER_TIMEOUT);
    UpdateYawController(1000 / PWM_FREQUENCY);
    UpdateHeightController(1000 / PWM_FREQUENCY);
}

void TimerInit(void) {
    SysCtlPeripheralEnable(TIMER_PERIPH);
    TimerConfigure(TIMER_BASE, TIMER_CONFIG);
    TimerLoadSet(TIMER_BASE, TIMER_TIMER, SysCtlClockGet() / PWM_FREQUENCY);

    TimerIntRegister(TIMER_BASE, TIMER_TIMER, TimerHandler);

    /*
     * Setup the interrupts for the timer timeouts.
     */
    IntEnable(TIMER_INT);
    TimerIntEnable(TIMER_BASE, TIMER_TIMEOUT);

    /*
     * Trigger ADC to capture height.
     */
    TimerADCEventSet(TIMER_BASE, TIMER_TIMEOUT);
    TimerControlTrigger(TIMER_BASE, TIMER_TIMER, true);

    /*
     * Enable the timers.
     */
    TimerEnable(TIMER_BASE, TIMER_TIMER);
}

void PriorityTaskInit(void) {
    TimerInit();
}

void PriorityTaskDisable(void) {
    TimerIntDisable(TIMER_BASE, TIMER_TIMEOUT);
}

void PriorityTaskEnable(void) {
    TimerIntEnable(TIMER_BASE, TIMER_TIMEOUT);
}

void FlightControllerInit(void) {
    PwmInit();
    SetTargetHeight(0);
    SetTargetYawDegrees(0);
    YawControllerInit();
    HeightControllerInit();
    PriorityTaskInit();
    ResetError();
}

void UpdateError(void) {
    static uint32_t idx = 0;
    uint16_t yaw_sample_err = abs(GetYaw() - GetTargetYaw());
    uint16_t height_sample_err = abs(
            GetHeightPercentage() - (int32_t) GetTargetHeight());
    yaw_error_buf[idx] = yaw_sample_err;
    height_error_buf[idx] = height_sample_err;
    idx = (idx + 1) % NUM_ERROR_SAMPLES;
}

void ResetError(void) {
    for (uint8_t i = 0; i < NUM_ERROR_SAMPLES; i++) {
        yaw_error_buf[i] = yaw_tolerance;
        height_error_buf[i] = height_tolerance;
    }
}

bool HasReachedTargetYaw(void) {
    uint16_t err_sum = 0;
    for (uint8_t i = 0; i < NUM_ERROR_SAMPLES; i++) {
        err_sum += yaw_error_buf[i];
    }
    return err_sum <= yaw_tolerance;
}

bool HasReachedTargetHeight(void) {
    uint16_t err_sum = 0;
    for (uint8_t i = 0; i < NUM_ERROR_SAMPLES; i++) {
        err_sum += height_error_buf[i];
    }
    return err_sum <= height_tolerance;
}

void UpdateFlightMode() {
    static bool wait = false;
    static bool wait_2 = false;
    static uint32_t elapsed_ticks = 0;
    bool event = GetSwitchEvent();
    uint8_t presses[NUM_BUTTONS];
    int32_t target_yaw;
    int32_t target_height;

    switch (flight_state) {

    case LANDED: {
        if (event == SWITCH_UP) {
            /*
             * Go to INIT state
             */
            flight_state = INIT;
        }
        break;
    }
    case INIT: {
        if (YawRefFound()) {
            wait = false;
            /*
             * Before entering the FLYING state must enable PWM, clear the pid controllers, and
             * enable the priority task scheduler.
             */
            YawControllerInit();
            HeightControllerInit();
            PwmEnable(MAIN_ROTOR);
            PwmEnable(TAIL_ROTOR);
            PriorityTaskEnable();
            ResetPushes();
            /*
             * Go to the FLYING state
             */
            flight_state = FLYING;
        } else if (!wait) {
            wait = true;
            YawRefTrigger();
            ZeroHeightTrigger();
            PriorityTaskDisable();
            SetPwmDutyCycle(MAIN_ROTOR, 25);
            PwmEnable(MAIN_ROTOR);
        }
        break;
    }

    case FLYING: {
        if (event == SWITCH_DOWN) {
            /*
             * Go to LANDING state
             */
            flight_state = LANDING;
        } else {
            /*
             * Get all the button pushes
             */
            for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
                presses[i] = NumPushes(i);
            }

            /*
             * Increase height
             */
            if (presses[BTN_UP] > 0) {
                /*
                 * If the helicopter is set to be at zero height, preload the integral
                 * so the rise time is less long.
                 */
                if (GetTargetHeight() == 0) {
                    PreloadHeightController(20, height_inc);
                }
                target_height = GetTargetHeight()
                        + presses[BTN_UP] * height_inc;
                target_height =
                        (target_height > height_max) ?
                                height_max : target_height;
                SetTargetHeight(target_height);
            }

            /*
             * Decrease height
             */
            if (presses[BTN_DOWN] > 0) {
                target_height = GetTargetHeight()
                        - presses[BTN_DOWN] * height_inc;
                target_height =
                        (target_height < height_min) ?
                                height_min : target_height;
                SetTargetHeight(target_height);
            }

            /*
             * Ignore yaw commands if the helicopter is at 0 height
             */
            if (GetTargetHeight() > 0) {
                /*
                 * Rotate counter-clockwise
                 */
                if (presses[BTN_LEFT] > 0) {
                    target_yaw = GetTargetYawDegrees()
                            - presses[BTN_LEFT] * yaw_inc;
                    SetTargetYawDegrees(target_yaw);
                }

                /*
                 * Rotate clockwise
                 */
                if (presses[BTN_RIGHT] > 0) {
                    target_yaw = GetTargetYawDegrees()
                            + presses[BTN_RIGHT] * yaw_inc;
                    SetTargetYawDegrees(target_yaw);
                }
            }
        }
        break;
    }

    case LANDING: {
        UpdateError();
        bool is_target_height_reached = HasReachedTargetHeight();
        bool is_target_yaw_reached = HasReachedTargetYaw();

        if (!wait) {
            /*
             * Wait until yaw is at closest reference.
             */
            wait = true;
            int32_t yaw_ref = GetClosestYawRef(target_yaw);
            SetTargetYaw(yaw_ref);

            /*
             * Reset the error mechanism used to detect if target yaw and height have been reached.
             */
            ResetError();
            elapsed_ticks = SchedulerTickCountGet();
        } else if (!wait_2 && is_target_yaw_reached) {
            wait_2 = true;
        } else {
            if (GetTargetHeight() == 0) {
                /*
                 * If 10 seconds have elapsed since it reached target height go to LANDED state
                 * regardless of yaw.
                 */
                if (is_target_height_reached
                        && (is_target_yaw_reached
                                || (SchedulerElapsedTicksGet(elapsed_ticks)
                                        * (1000 / PWM_FREQUENCY) > 10000))) {
                    wait = false;
                    wait_2 = false;
                    PwmDisable(MAIN_ROTOR);
                    PwmDisable(TAIL_ROTOR);
                    /*
                     * Go to the LANDED state after disabling PWM
                     */
                    flight_state = LANDED;
                }

            } else {
                if (wait_2
                        && ((SchedulerElapsedTicksGet(elapsed_ticks)
                                * (1000 / PWM_FREQUENCY)) >= RATE_OF_DESCENT)) {
                    elapsed_ticks = SchedulerTickCountGet();
                    SetTargetHeight(GetTargetHeight() - 1);
                }
            }
        }
        break;
    }

    }
}

const char* GetFlightMode(void) {
    return flight_mode[flight_state];
}
