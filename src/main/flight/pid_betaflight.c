/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include <platform.h>

#ifndef SKIP_PID_FLOAT

#include "build/build_config.h"
#include "build/debug.h"

#include "common/axis.h"
#include "common/maths.h"
#include "common/filter.h"

#include "drivers/sensor.h"

#include "drivers/accgyro.h"
#include "sensors/sensors.h"
#include "sensors/gyro.h"
#include "sensors/acceleration.h"

#include "rx/rx.h"

#include "io/gps.h"

#include "fc/rc_controls.h"
#include "fc/runtime_config.h"

#include "flight/pid.h"
#include "flight/imu.h"
#include "flight/navigation.h"
#include "flight/gtune.h"

extern float rcInput[3];
extern float setpointRate[3];

extern float errorGyroIf[3];
extern bool pidStabilisationEnabled;

extern pt1Filter_t deltaFilter[3];
extern pt1Filter_t yawFilter;
extern biquadFilter_t dtermFilterLpf[3];
extern biquadFilter_t dtermFilterNotch[3];
extern bool dtermNotchInitialised;
extern firFilterState_t dtermDenoisingState[3];

void initFilters(const pidProfile_t *pidProfile);
float getdT(void);

// Betaflight pid controller, which will be maintained in the future with additional features specialised for current (mini) multirotor usage.
// Based on 2DOF reference design (matlab)
void pidBetaflight(const pidProfile_t *pidProfile, uint16_t max_angle_inclination,
         const rollAndPitchTrims_t *angleTrim, const rxConfig_t *rxConfig)
{
    float errorRate = 0, rD = 0, PVRate = 0, dynC;
    float ITerm,PTerm,DTerm;
    static float lastRateError[2];
    static float Kp[3], Ki[3], Kd[3], c[3], rollPitchMaxVelocity, yawMaxVelocity, previousSetpoint[3], relaxFactor[3];
    float delta;
    int axis;
    float horizonLevelStrength = 1;

    float tpaFactor = PIDweight[0] / 100.0f; // tpa is now float

    initFilters(pidProfile);

    if (FLIGHT_MODE(HORIZON_MODE)) {
        // Figure out the raw stick positions
        const int32_t stickPosAil = ABS(getRcStickDeflection(FD_ROLL, rxConfig->midrc));
        const int32_t stickPosEle = ABS(getRcStickDeflection(FD_PITCH, rxConfig->midrc));
        const int32_t mostDeflectedPos = MAX(stickPosAil, stickPosEle);
        // Progressively turn off the horizon self level strength as the stick is banged over
        horizonLevelStrength = (float)(500 - mostDeflectedPos) / 500;  // 1 at centre stick, 0 = max stick deflection
        if(pidProfile->D8[PIDLEVEL] == 0){
            horizonLevelStrength = 0;
        } else {
            horizonLevelStrength = constrainf(((horizonLevelStrength - 1) * (100 / pidProfile->D8[PIDLEVEL])) + 1, 0, 1);
        }
    }

    // Yet Highly experimental and under test and development
    // Throttle coupled to Igain like inverted TPA // 50hz calculation (should cover all rx protocols)
    static float kiThrottleGain = 1.0f;
    if (pidProfile->itermThrottleGain) {
        const uint16_t maxLoopCount = 20000 / targetPidLooptime;
        const float throttleItermGain = (float)pidProfile->itermThrottleGain * 0.001f;
        static int16_t previousThrottle;
        static uint16_t loopIncrement;

        if (loopIncrement >= maxLoopCount) {
            kiThrottleGain = 1.0f + constrainf((float)(ABS(rcCommand[THROTTLE] - previousThrottle)) * throttleItermGain, 0.0f, 5.0f); // Limit to factor 5
            previousThrottle = rcCommand[THROTTLE];
            loopIncrement = 0;
        } else {
            loopIncrement++;
        }
    }

    // ----------PID controller----------
    for (axis = 0; axis < 3; axis++) {

        static uint8_t configP[3], configI[3], configD[3];

        // Prevent unnecessary computing and check for changed PIDs. No need for individual checks. Only pids is fine for now
        // Prepare all parameters for PID controller
        if ((pidProfile->P8[axis] != configP[axis]) || (pidProfile->I8[axis] != configI[axis]) || (pidProfile->D8[axis] != configD[axis])) {
            Kp[axis] = PTERM_SCALE * pidProfile->P8[axis];
            Ki[axis] = ITERM_SCALE * pidProfile->I8[axis];
            Kd[axis] = DTERM_SCALE * pidProfile->D8[axis];
            c[axis] = pidProfile->dtermSetpointWeight / 100.0f;
            relaxFactor[axis] = 1.0f - (pidProfile->setpointRelaxRatio / 100.0f);
            yawMaxVelocity = pidProfile->yawRateAccelLimit * 1000 * getdT();
            rollPitchMaxVelocity = pidProfile->rateAccelLimit * 1000 * getdT();

            configP[axis] = pidProfile->P8[axis];
            configI[axis] = pidProfile->I8[axis];
            configD[axis] = pidProfile->D8[axis];
        }

        // Limit abrupt yaw inputs / stops
        float maxVelocity = (axis == YAW) ? yawMaxVelocity : rollPitchMaxVelocity;
        if (maxVelocity) {
            float currentVelocity = setpointRate[axis] - previousSetpoint[axis];
            if (ABS(currentVelocity) > maxVelocity) {
                setpointRate[axis] = (currentVelocity > 0) ? previousSetpoint[axis] + maxVelocity : previousSetpoint[axis] - maxVelocity;
            }
            previousSetpoint[axis] = setpointRate[axis];
        }

        // Yaw control is GYRO based, direct sticks control is applied to rate PID
        if ((FLIGHT_MODE(ANGLE_MODE) || FLIGHT_MODE(HORIZON_MODE)) && axis != YAW) {
            // calculate error angle and limit the angle to the max inclination
#ifdef GPS
                const float errorAngle = (constrainf(pidProfile->levelSensitivity * rcCommand[axis] + GPS_angle[axis], -((int) max_angle_inclination),
                    +max_angle_inclination) - attitude.raw[axis] + angleTrim->raw[axis]) / 10.0f;
#else
                const float errorAngle = (constrainf(pidProfile->levelSensitivity * rcCommand[axis], -((int) max_angle_inclination),
                    +max_angle_inclination) - attitude.raw[axis] + angleTrim->raw[axis]) / 10.0f;
#endif
            if (FLIGHT_MODE(ANGLE_MODE)) {
                // ANGLE mode - control is angle based, so control loop is needed
                setpointRate[axis] = errorAngle * pidProfile->P8[PIDLEVEL] / 10.0f;
            } else {
                // HORIZON mode - direct sticks control is applied to rate PID
                // mix up angle error to desired AngleRate to add a little auto-level feel
                setpointRate[axis] = setpointRate[axis] + (errorAngle * pidProfile->I8[PIDLEVEL] * horizonLevelStrength / 10.0f);
            }
        }

        PVRate = gyroADCf[axis] / 16.4f; // Process variable from gyro output in deg/sec

        // --------low-level gyro-based PID based on 2DOF PID controller. ----------
        //  ---------- 2-DOF PID controller with optional filter on derivative term. b = 1 and only c can be tuned (amount derivative on measurement or error).  ----------
        // Used in stand-alone mode for ACRO, controlled by higher level regulators in other modes
        // ----- calculate error / angle rates  ----------
        errorRate = setpointRate[axis] - PVRate;       // r - y

        // -----calculate P component and add Dynamic Part based on stick input
        PTerm = Kp[axis] * errorRate * tpaFactor;

        // -----calculate I component.
        // Reduce strong Iterm accumulation during higher stick inputs
        float accumulationThreshold = (axis == YAW) ? pidProfile->yawItermIgnoreRate : pidProfile->rollPitchItermIgnoreRate;
        float setpointRateScaler = constrainf(1.0f - (ABS(setpointRate[axis]) / accumulationThreshold), 0.0f, 1.0f);

        // Handle All windup Scenarios
        // limit maximum integrator value to prevent WindUp
        float itermScaler = setpointRateScaler * kiThrottleGain;

        errorGyroIf[axis] = constrainf(errorGyroIf[axis] + Ki[axis] * errorRate * getdT() * itermScaler, -250.0f, 250.0f);

        // I coefficient (I8) moved before integration to make limiting independent from PID settings
        ITerm = errorGyroIf[axis];

        //-----calculate D-term (Yaw D not yet supported)
        if (axis != YAW) {
            if (pidProfile->setpointRelaxRatio < 100)
                dynC = c[axis] * powerf(rcInput[axis], 2) * relaxFactor[axis] + c[axis] * (1-relaxFactor[axis]);
            else
                dynC = c[axis];
            rD = dynC * setpointRate[axis] - PVRate;    // cr - y
            delta = rD - lastRateError[axis];
            lastRateError[axis] = rD;

            // Divide delta by targetLooptime to get differential (ie dr/dt)
            delta *= (1.0f / getdT());

            if (debugMode == DEBUG_DTERM_FILTER) debug[axis] = Kd[axis] * delta * tpaFactor;

            // Filter delta
            if (dtermNotchInitialised) delta = biquadFilterApply(&dtermFilterNotch[axis], delta);

            if (pidProfile->dterm_lpf_hz) {
                if (pidProfile->dterm_filter_type == FILTER_BIQUAD)
                    delta = biquadFilterApply(&dtermFilterLpf[axis], delta);
                else if (pidProfile->dterm_filter_type == FILTER_PT1)
                    delta = pt1FilterApply4(&deltaFilter[axis], delta, pidProfile->dterm_lpf_hz, getdT());
                else
                    delta = firFilterUpdate(&dtermDenoisingState[axis], delta);
            }

            DTerm = Kd[axis] * delta * tpaFactor;

            // -----calculate total PID output
            axisPID[axis] = constrain(lrintf(PTerm + ITerm + DTerm), -pidProfile->pidSumLimit, pidProfile->pidSumLimit);
        } else {
            if (pidProfile->yaw_lpf_hz) PTerm = pt1FilterApply4(&yawFilter, PTerm, pidProfile->yaw_lpf_hz, getdT());

            axisPID[axis] = lrintf(PTerm + ITerm);

            DTerm = 0.0f; // needed for blackbox
        }

        // Disable PID control at zero throttle
        if (!pidStabilisationEnabled) axisPID[axis] = 0;

#ifdef GTUNE
        if (FLIGHT_MODE(GTUNE_MODE) && ARMING_FLAG(ARMED)) {
            calculate_Gtune(axis);
        }
#endif

#ifdef BLACKBOX
        axisPID_P[axis] = PTerm;
        axisPID_I[axis] = ITerm;
        axisPID_D[axis] = DTerm;
#endif
    }
}
#endif

