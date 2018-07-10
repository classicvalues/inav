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

/*
 Created by Marcin Baliniak
 some functions based on MinimOSD

 OSD-CMS separation by jflyper
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "platform.h"

#ifdef USE_OSD

#include "build/debug.h"
#include "build/version.h"

#include "cms/cms.h"
#include "cms/cms_types.h"
#include "cms/cms_menu_osd.h"

#include "common/axis.h"
#include "common/filter.h"
#include "common/printf.h"
#include "common/string_light.h"
#include "common/time.h"
#include "common/typeconversion.h"
#include "common/utils.h"

#include "config/feature.h"
#include "config/parameter_group.h"
#include "config/parameter_group_ids.h"

#include "drivers/display.h"
#include "drivers/max7456_symbols.h"
#include "drivers/time.h"
#include "drivers/vtx_common.h"

#include "io/flashfs.h"
#include "io/gps.h"
#include "io/osd.h"
#include "io/vtx_string.h"

#include "fc/fc_core.h"
#include "fc/config.h"
#include "fc/controlrate_profile.h"
#include "fc/rc_adjustments.h"
#include "fc/rc_controls.h"
#include "fc/rc_modes.h"
#include "fc/runtime_config.h"
#include "fc/fc_tasks.h"

#include "flight/imu.h"
#include "flight/mixer.h"
#include "flight/pid.h"
#include "flight/rth_estimator.h"
#include "flight/wind_estimator.h"

#include "navigation/navigation.h"

#include "rx/rx.h"

#include "sensors/battery.h"
#include "sensors/boardalignment.h"
#include "sensors/diagnostics.h"
#include "sensors/sensors.h"
#include "sensors/pitotmeter.h"

#ifdef USE_HARDWARE_REVISION_DETECTION
#include "hardware_revision.h"
#endif

#define VIDEO_BUFFER_CHARS_PAL    480
#define IS_DISPLAY_PAL (displayScreenSize(osdDisplayPort) == VIDEO_BUFFER_CHARS_PAL)

#define CENTIMETERS_TO_CENTIFEET(cm)            (cm * (328 / 100.0))
#define CENTIMETERS_TO_FEET(cm)                 (cm * (328 / 10000.0))
#define CENTIMETERS_TO_METERS(cm)               (cm / 100)
#define FEET_PER_MILE                           5280
#define FEET_PER_KILOFEET                       1000 // Used for altitude
#define METERS_PER_KILOMETER                    1000
#define METERS_PER_MILE                         1609

#define DELAYED_REFRESH_RESUME_COMMAND (checkStickPosition(THR_HI) || checkStickPosition(PIT_HI))

#define SPLASH_SCREEN_DISPLAY_TIME 4000 // ms
#define ARMED_SCREEN_DISPLAY_TIME 1500 // ms
#define STATS_SCREEN_DISPLAY_TIME 60000 // ms

#define EFFICIENCY_UPDATE_INTERVAL (5 * 1000)

// Adjust OSD_MESSAGE's default position when
// changing OSD_MESSAGE_LENGTH
#define OSD_MESSAGE_LENGTH 28
#define OSD_ALTERNATING_CHOICES(ms, num_choices) ((millis() / ms) % num_choices)
#define _CONST_STR_SIZE(s) ((sizeof(s)/sizeof(s[0]))-1) // -1 to avoid counting final '\0'
// Wrap all string constants intenteded for display as messages with
// this macro to ensure compile time length validation.
#define OSD_MESSAGE_STR(x) ({ \
    STATIC_ASSERT(_CONST_STR_SIZE(x) <= OSD_MESSAGE_LENGTH, message_string_ ## __COUNTER__ ## _too_long); \
    x; \
})

static unsigned currentLayout = 0;
static int layoutOverride = -1;

typedef struct statistic_s {
    uint16_t max_speed;
    uint16_t min_voltage; // /100
    int16_t max_current; // /100
    int16_t max_power; // /100
    int16_t min_rssi;
    int32_t max_altitude;
    uint16_t max_distance;
} statistic_t;

static statistic_t stats;

typedef enum {
    OSD_SIDEBAR_ARROW_NONE,
    OSD_SIDEBAR_ARROW_UP,
    OSD_SIDEBAR_ARROW_DOWN,
} osd_sidebar_arrow_e;

typedef struct osd_sidebar_s {
    int32_t offset;
    timeMs_t updated;
    osd_sidebar_arrow_e arrow;
    uint8_t idle;
} osd_sidebar_t;

static timeUs_t resumeRefreshAt = 0;
static bool refreshWaitForResumeCmdRelease;

static bool fullRedraw = false;

static uint8_t armState;

static displayPort_t *osdDisplayPort;

#define AH_MAX_PITCH 200 // Specify maximum AHI pitch value displayed. Default 200 = 20.0 degrees
#define AH_MAX_ROLL 400  // Specify maximum AHI roll value displayed. Default 400 = 40.0 degrees
#define AH_SYMBOL_COUNT 9
#define AH_SIDEBAR_WIDTH_POS 7
#define AH_SIDEBAR_HEIGHT_POS 3

PG_REGISTER_WITH_RESET_FN(osdConfig_t, osdConfig, PG_OSD_CONFIG, 3);

static int digitCount(int32_t value)
{
    int digits = 1;
    while(1) {
        value = value / 10;
        if (value == 0) {
            break;
        }
        digits++;
    }
    return digits;
}

/**
 * Formats a number given in cents, to support non integer values
 * without using floating point math. Value is always right aligned
 * and spaces are inserted before the number to always yield a string
 * of the same length. If the value doesn't fit into the provided length
 * it will be divided by scale and true will be returned.
 */
 static bool osdFormatCentiNumber(char *buff, int32_t centivalue, uint32_t scale, int maxDecimals, int maxScaledDecimals, int length)
 {
    char *ptr = buff;
    char *dec;
    int decimals = maxDecimals;
    bool negative = false;
    bool scaled = false;

    buff[length] = '\0';

    if (centivalue < 0) {
        negative = true;
        centivalue = -centivalue;
        length--;
    }

    int32_t integerPart = centivalue / 100;
    // 3 decimal digits
    int32_t millis = (centivalue % 100) * 10;

    int digits = digitCount(integerPart);
    int remaining = length - digits;

    if (remaining < 0 && scale > 0) {
        // Reduce by scale
        scaled = true;
        decimals = maxScaledDecimals;
        integerPart = integerPart / scale;
        // Multiply by 10 to get 3 decimal digits
        millis = ((centivalue % (100 * scale)) * 10) / scale;
        digits = digitCount(integerPart);
        remaining = length - digits;
    }

    // 3 decimals at most
    decimals = MIN(remaining, MIN(decimals, 3));
    remaining -= decimals;

    // Done counting. Time to write the characters.

    // Write spaces at the start
    while (remaining > 0) {
        *ptr = SYM_BLANK;
        ptr++;
        remaining--;
    }

    // Write the minus sign if required
    if (negative) {
        *ptr = '-';
        ptr++;
    }
    // Now write the digits.
    ui2a(integerPart, 10, 0, ptr);
    ptr += digits;
    if (decimals > 0) {
        *(ptr-1) += SYM_ZERO_HALF_TRAILING_DOT - '0';
        dec = ptr;
        int factor = 3; // we're getting the decimal part in millis first
        while (decimals < factor) {
            factor--;
            millis /= 10;
        }
        int decimalDigits = digitCount(millis);
        while (decimalDigits < decimals) {
            decimalDigits++;
            *ptr = '0';
            ptr++;
        }
        ui2a(millis, 10, 0, ptr);
        *dec += SYM_ZERO_HALF_LEADING_DOT - '0';
    }
    return scaled;
}

/**
 * Converts distance into a string based on the current unit system
 * prefixed by a a symbol to indicate the unit used.
 * @param dist Distance in centimeters
 */
static void osdFormatDistanceSymbol(char *buff, int32_t dist)
{
    switch ((osd_unit_e)osdConfig()->units) {
    case OSD_UNIT_IMPERIAL:
        if (osdFormatCentiNumber(buff + 1, CENTIMETERS_TO_CENTIFEET(dist), FEET_PER_MILE, 0, 3, 3)) {
            buff[0] = SYM_DIST_MI;
        } else {
            buff[0] = SYM_DIST_FT;
        }
        break;
    case OSD_UNIT_UK:
        FALLTHROUGH;
    case OSD_UNIT_METRIC:
        if (osdFormatCentiNumber(buff + 1, dist, METERS_PER_KILOMETER, 0, 3, 3)) {
            buff[0] = SYM_DIST_KM;
        } else {
            buff[0] = SYM_DIST_M;
        }
        break;
    }
}

/**
 * Converts distance into a string based on the current unit system.
 * @param dist Distance in centimeters
 */
 static void osdFormatDistanceStr(char *buff, int32_t dist)
 {
     int32_t centifeet;
     switch ((osd_unit_e)osdConfig()->units) {
     case OSD_UNIT_IMPERIAL:
        centifeet = CENTIMETERS_TO_CENTIFEET(dist);
        if (abs(centifeet) < FEET_PER_MILE * 100 / 2) {
            // Show feet when dist < 0.5mi
            tfp_sprintf(buff, "%d%c", centifeet / 100, SYM_FT);
        } else {
            // Show miles when dist >= 0.5mi
            tfp_sprintf(buff, "%d.%02d%c", centifeet / (100*FEET_PER_MILE),
            (abs(centifeet) % (100 * FEET_PER_MILE)) / FEET_PER_MILE, SYM_MI);
        }
        break;
     case OSD_UNIT_UK:
         FALLTHROUGH;
     case OSD_UNIT_METRIC:
        if (abs(dist) < METERS_PER_KILOMETER * 100) {
            // Show meters when dist < 1km
            tfp_sprintf(buff, "%d%c", dist / 100, SYM_M);
        } else {
            // Show kilometers when dist >= 1km
            tfp_sprintf(buff, "%d.%02d%c", dist / (100*METERS_PER_KILOMETER),
                (abs(dist) % (100 * METERS_PER_KILOMETER)) / METERS_PER_KILOMETER, SYM_KM);
         }
         break;
     }
 }

/**
 * Converts velocity based on the current unit system (kmh or mph).
 * @param alt Raw velocity (i.e. as taken from gpsSol.groundSpeed in centimeters/second)
 */
static int32_t osdConvertVelocityToUnit(int32_t vel)
{
    switch ((osd_unit_e)osdConfig()->units) {
    case OSD_UNIT_UK:
        FALLTHROUGH;
    case OSD_UNIT_IMPERIAL:
        return (vel * 224) / 10000; // Convert to mph
    case OSD_UNIT_METRIC:
        return (vel * 36) / 1000;   // Convert to kmh
    }
    // Unreachable
    return -1;
}

/**
 * Converts velocity into a string based on the current unit system.
 * @param alt Raw velocity (i.e. as taken from gpsSol.groundSpeed in centimeters/seconds)
 */
static void osdFormatVelocityStr(char* buff, int32_t vel)
{
    switch ((osd_unit_e)osdConfig()->units) {
    case OSD_UNIT_UK:
        FALLTHROUGH;
    case OSD_UNIT_IMPERIAL:
        tfp_sprintf(buff, "%3d%c", osdConvertVelocityToUnit(vel), SYM_MPH);
        break;
    case OSD_UNIT_METRIC:
        tfp_sprintf(buff, "%3d%c", osdConvertVelocityToUnit(vel), SYM_KMH);
        break;
    }
}

/**
 * Converts wind speed into a string based on the current unit system, using
 * always 3 digits and an additional character for the unit at the right. buff
 * is null terminated.
 * @param ws Raw wind speed in cm/s
 */
#ifdef USE_WIND_ESTIMATOR
static void osdFormatWindSpeedStr(char *buff, int32_t ws, bool isValid)
{
    int32_t centivalue;
    char suffix;
    switch (osdConfig()->units) {
        case OSD_UNIT_UK:
            FALLTHROUGH;
        case OSD_UNIT_IMPERIAL:
            centivalue = (ws * 224) / 100;
            suffix = SYM_MPH;
            break;
        case OSD_UNIT_METRIC:
            centivalue = (ws * 36) / 10;
            suffix = SYM_KMH;
            break;
    }
    if (isValid) {
        osdFormatCentiNumber(buff, centivalue, 0, 2, 0, 3);
    } else {
        buff[0] = buff[1] = buff[2] = '-';
    }
    buff[3] = suffix;
    buff[4] = '\0';
}
#endif

/**
* Converts altitude into a string based on the current unit system
* prefixed by a a symbol to indicate the unit used.
* @param alt Raw altitude/distance (i.e. as taken from baro.BaroAlt in centimeters)
*/
static void osdFormatAltitudeSymbol(char *buff, int32_t alt)
{
    switch ((osd_unit_e)osdConfig()->units) {
        case OSD_UNIT_IMPERIAL:
            if (osdFormatCentiNumber(buff + 1, CENTIMETERS_TO_CENTIFEET(alt), 1000, 0, 2, 3)) {
                // Scaled to kft
                buff[0] = SYM_ALT_KFT;
            } else {
                // Formatted in feet
                buff[0] = SYM_ALT_FT;
            }
            break;
        case OSD_UNIT_UK:
            FALLTHROUGH;
        case OSD_UNIT_METRIC:
            // alt is alredy in cm
            if (osdFormatCentiNumber(buff+1, alt, 1000, 0, 2, 3)) {
                // Scaled to km
                buff[0] = SYM_ALT_KM;
            } else {
                // Formatted in m
                buff[0] = SYM_ALT_M;
            }
            break;
    }
}

/**
* Converts altitude into a string based on the current unit system.
* @param alt Raw altitude/distance (i.e. as taken from baro.BaroAlt in centimeters)
*/
static void osdFormatAltitudeStr(char *buff, int32_t alt)
{
    int32_t value;
    switch ((osd_unit_e)osdConfig()->units) {
        case OSD_UNIT_IMPERIAL:
            value = CENTIMETERS_TO_FEET(alt);
            tfp_sprintf(buff, "%d%c", value, SYM_FT);
            break;
        case OSD_UNIT_UK:
            FALLTHROUGH;
        case OSD_UNIT_METRIC:
            value = CENTIMETERS_TO_METERS(alt);
            tfp_sprintf(buff, "%d%c", value, SYM_M);
            break;
    }
}

static void osdFormatTime(char *buff, uint32_t seconds, char sym_m, char sym_h)
{
    uint32_t value = seconds;
    char sym = sym_m;
    // Maximum value we can show in minutes is 99 minutes and 59 seconds
    if (seconds > (99 * 60) + 59) {
        sym = sym_h;
        value = seconds / 60;
    }
    buff[0] = sym;
    tfp_sprintf(buff + 1, "%02d:%02d", value / 60, value % 60);
}

static inline void osdFormatOnTime(char *buff)
{
    osdFormatTime(buff, micros() / 1000000, SYM_ON_M, SYM_ON_H);
}

static inline void osdFormatFlyTime(char *buff, textAttributes_t *attr)
{
    uint32_t seconds = getFlightTime();
    osdFormatTime(buff, seconds, SYM_FLY_M, SYM_FLY_H);
    if (attr && osdConfig()->time_alarm > 0) {
       if (seconds / 60 >= osdConfig()->time_alarm && ARMING_FLAG(ARMED)) {
            TEXT_ATTRIBUTES_ADD_BLINK(*attr);
        }
    }
}

/**
 * Converts RSSI into a % value used by the OSD.
 */
static uint16_t osdConvertRSSI(void)
{
    uint16_t osdRssi = getRSSI() * 100 / 1024; // change range
    if (osdRssi >= 100) {
        osdRssi = 99;
    }
    return osdRssi;
}

static void osdFormatCoordinate(char *buff, char sym, int32_t val)
{
    const int coordinateMaxLength = 12; // 11 for the number + 1 for the symbol

    buff[0] = sym;
    int32_t integerPart = val / GPS_DEGREES_DIVIDER;
    int32_t decimalPart = abs(val % GPS_DEGREES_DIVIDER);
    // Latitude maximum integer width is 3 (-90) while
    // longitude maximum integer width is 4 (-180). We always
    // show 7 decimals, so we need to use 11 spaces because
    // we're embedding the decimal separator between the
    // two numbers.
    int written = tfp_sprintf(buff + 1, "%d", integerPart);
    tfp_sprintf(buff + 1 + written, "%07d", decimalPart);
    // Embbed the decimal separator
    buff[1+written-1] += SYM_ZERO_HALF_TRAILING_DOT - '0';
    buff[1+written] += SYM_ZERO_HALF_LEADING_DOT - '0';
    // Pad to 11 coordinateMaxLength
    while(1 + 7 + written < coordinateMaxLength) {
        buff[1 + 7 + written] = SYM_BLANK;
        written++;
    }
    buff[coordinateMaxLength] = '\0';
}

// Used twice, make sure it's exactly the same string
// to save some memory
#define RC_RX_LINK_LOST_MSG "!RC RX LINK LOST!"

static const char * osdArmingDisabledReasonMessage(void)
{
    switch (isArmingDisabledReason()) {
        case ARMING_DISABLED_FAILSAFE_SYSTEM:
            // See handling of FAILSAFE_RX_LOSS_MONITORING in failsafe.c
            if (failsafePhase() == FAILSAFE_RX_LOSS_MONITORING) {
                if (failsafeIsReceivingRxData()) {
                    if (isUsingSticksForArming()) {
                        // Need to power-cycle
                        return OSD_MESSAGE_STR("POWER CYCLE TO ARM");
                    }
                    // If we're not using sticks, it means the ARM switch
                    // hasn't been off since entering FAILSAFE_RX_LOSS_MONITORING
                    // yet
                    return OSD_MESSAGE_STR("TURN ARM SWITCH OFF");
                }
                // Not receiving RX data
                return OSD_MESSAGE_STR(RC_RX_LINK_LOST_MSG);
            }
            return OSD_MESSAGE_STR("DISABLED BY FAILSAFE");
        case ARMING_DISABLED_NOT_LEVEL:
            return OSD_MESSAGE_STR("AIRCRAFT IS NOT LEVEL");
        case ARMING_DISABLED_SENSORS_CALIBRATING:
            return OSD_MESSAGE_STR("SENSORS CALIBRATING");
        case ARMING_DISABLED_SYSTEM_OVERLOADED:
            return OSD_MESSAGE_STR("SYSTEM OVERLOADED");
        case ARMING_DISABLED_NAVIGATION_UNSAFE:
            return OSD_MESSAGE_STR("NAVIGATION IS UNSAFE");
        case ARMING_DISABLED_COMPASS_NOT_CALIBRATED:
            return OSD_MESSAGE_STR("COMPASS NOT CALIBRATED");
        case ARMING_DISABLED_ACCELEROMETER_NOT_CALIBRATED:
            return OSD_MESSAGE_STR("ACCELEROMETER NOT CALIBRATED");
        case ARMING_DISABLED_ARM_SWITCH:
            return OSD_MESSAGE_STR("DISABLE ARM SWITCH FIRST");
        case ARMING_DISABLED_HARDWARE_FAILURE:
            {
                if (!HW_SENSOR_IS_HEALTHY(getHwGyroStatus())) {
                    return OSD_MESSAGE_STR("GYRO FAILURE");
                }
                if (!HW_SENSOR_IS_HEALTHY(getHwAccelerometerStatus())) {
                    return OSD_MESSAGE_STR("ACCELEROMETER FAILURE");
                }
                if (!HW_SENSOR_IS_HEALTHY(getHwCompassStatus())) {
                    return OSD_MESSAGE_STR("COMPASS FAILURE");
                }
                if (!HW_SENSOR_IS_HEALTHY(getHwBarometerStatus())) {
                    return OSD_MESSAGE_STR("BAROMETER FAILURE");
                }
                if (!HW_SENSOR_IS_HEALTHY(getHwGPSStatus())) {
                    return OSD_MESSAGE_STR("GPS FAILURE");
                }
                if (!HW_SENSOR_IS_HEALTHY(getHwRangefinderStatus())) {
                    return OSD_MESSAGE_STR("RANGE FINDER FAILURE");
                }
                if (!HW_SENSOR_IS_HEALTHY(getHwPitotmeterStatus())) {
                    return OSD_MESSAGE_STR("PITOT METER FAILURE");
                }
            }
            return OSD_MESSAGE_STR("HARDWARE FAILURE");
        case ARMING_DISABLED_BOXFAILSAFE:
            return OSD_MESSAGE_STR("FAILSAFE MODE ENABLED");
        case ARMING_DISABLED_BOXKILLSWITCH:
            return OSD_MESSAGE_STR("KILLSWITCH MODE ENABLED");
        case ARMING_DISABLED_RC_LINK:
            return OSD_MESSAGE_STR("NO RC LINK");
        case ARMING_DISABLED_THROTTLE:
            return OSD_MESSAGE_STR("THROTTLE IS NOT LOW");
        case ARMING_DISABLED_ROLLPITCH_NOT_CENTERED:
            return OSD_MESSAGE_STR("ROLLPITCH NOT CENTERED");
        case ARMING_DISABLED_SERVO_AUTOTRIM:
            return OSD_MESSAGE_STR("AUTOTRIM IS ACTIVE");
        case ARMING_DISABLED_OOM:
            return OSD_MESSAGE_STR("NOT ENOUGH MEMORY");
        case ARMING_DISABLED_CLI:
            return OSD_MESSAGE_STR("CLI IS ACTIVE");
            // Cases without message
        case ARMING_DISABLED_CMS_MENU:
            FALLTHROUGH;
        case ARMING_DISABLED_OSD_MENU:
            FALLTHROUGH;
        case ARMING_DISABLED_ALL_FLAGS:
            FALLTHROUGH;
        case ARMED:
            FALLTHROUGH;
        case WAS_EVER_ARMED:
            break;
    }
    return NULL;
}

static const char * osdFailsafePhaseMessage(void)
{
    // See failsafe.h for each phase explanation
    switch (failsafePhase()) {
#ifdef USE_NAV
        case FAILSAFE_RETURN_TO_HOME:
            // XXX: Keep this in sync with OSD_FLYMODE.
            return OSD_MESSAGE_STR("(RTH)");
#endif
        case FAILSAFE_LANDING:
            // This should be considered an emergengy landing
            return OSD_MESSAGE_STR("(EMERGENCY LANDING)");
        case FAILSAFE_RX_LOSS_MONITORING:
            // Only reachable from FAILSAFE_LANDED, which performs
            // a disarm. Since aircraft has been disarmed, we no
            // longer show failsafe details.
            FALLTHROUGH;
        case FAILSAFE_LANDED:
            // Very brief, disarms and transitions into
            // FAILSAFE_RX_LOSS_MONITORING. Note that it prevents
            // further rearming via ARMING_DISABLED_FAILSAFE_SYSTEM,
            // so we'll show the user how to re-arm in when
            // that flag is the reason to prevent arming.
            FALLTHROUGH;
        case FAILSAFE_RX_LOSS_IDLE:
            // This only happens when user has chosen NONE as FS
            // procedure. The recovery messages should be enough.
            FALLTHROUGH;
        case FAILSAFE_IDLE:
            // Failsafe not active
            FALLTHROUGH;
        case FAILSAFE_RX_LOSS_DETECTED:
            // Very brief, changes to FAILSAFE_RX_LOSS_RECOVERED
            // or the FS procedure immediately.
            FALLTHROUGH;
        case FAILSAFE_RX_LOSS_RECOVERED:
            // Exiting failsafe
            break;
    }
    return NULL;
}

static const char * osdFailsafeInfoMessage(void)
{
    if (failsafeIsReceivingRxData()) {
        // User must move sticks to exit FS mode
        return OSD_MESSAGE_STR("!MOVE STICKS TO EXIT FS!");
    }
    return OSD_MESSAGE_STR(RC_RX_LINK_LOST_MSG);
}

static const char * navigationStateMessage(void)
{
    switch (NAV_Status.state) {
        case MW_NAV_STATE_NONE:
            break;
        case MW_NAV_STATE_RTH_START:
            return OSD_MESSAGE_STR("STARTING RTH");
        case MW_NAV_STATE_RTH_ENROUTE:
            // TODO: Break this up between climb and head home
            return OSD_MESSAGE_STR("EN ROUTE TO HOME");
        case MW_NAV_STATE_HOLD_INFINIT:
            // Used by HOLD flight modes. No information to add.
            break;
        case MW_NAV_STATE_HOLD_TIMED:
            // Not used anymore
            break;
        case MW_NAV_STATE_WP_ENROUTE:
            // TODO: Show WP number
            return OSD_MESSAGE_STR("EN ROUTE TO WAYPOINT");
        case MW_NAV_STATE_PROCESS_NEXT:
            return OSD_MESSAGE_STR("PREPARING FOR NEXT WAYPOINT");
        case MW_NAV_STATE_DO_JUMP:
            // Not used
            break;
        case MW_NAV_STATE_LAND_START:
            return OSD_MESSAGE_STR("STARTING EMERGENCY LANDING");
        case MW_NAV_STATE_LAND_IN_PROGRESS:
            return OSD_MESSAGE_STR("LANDING");
        case MW_NAV_STATE_HOVER_ABOVE_HOME:
            if (STATE(FIXED_WING)) {
                return OSD_MESSAGE_STR("LOITERING AROUND HOME");
            }
            return OSD_MESSAGE_STR("HOVERING");
        case MW_NAV_STATE_LANDED:
            return OSD_MESSAGE_STR("LANDED");
        case MW_NAV_STATE_LAND_SETTLE:
            return OSD_MESSAGE_STR("PREPARING TO LAND");
        case MW_NAV_STATE_LAND_START_DESCENT:
            // Not used
            break;
    }
    return NULL;
}

static void osdFormatMessage(char *buff, size_t size, const char *message)
{
    memset(buff, SYM_BLANK, size);
    if (message) {
        int messageLength = strlen(message);
        int rem = MAX(0, OSD_MESSAGE_LENGTH - (int)messageLength);
        // Don't finish the string at the end of the message,
        // write the rest of the blanks.
        strncpy(buff + rem / 2, message, MIN(OSD_MESSAGE_LENGTH - rem / 2, messageLength));
    }
    // Ensure buff is zero terminated
    buff[size - 1] = '\0';
}

/**
 * Draws the battery symbol filled in accordingly to the
 * battery voltage to buff[0].
 **/
static void osdFormatBatteryChargeSymbol(char *buff)
{
    uint8_t p = calculateBatteryPercentage();
    p = (100 - p) / 16.6;
    buff[0] = SYM_BATT_FULL + p;
}

static void osdUpdateBatteryCapacityOrVoltageTextAttributes(textAttributes_t *attr)
{
    if ((getBatteryState() != BATTERY_NOT_PRESENT) && ((batteryUsesCapacityThresholds() && (getBatteryRemainingCapacity() <= currentBatteryProfile->capacity.warning - currentBatteryProfile->capacity.critical)) || ((!batteryUsesCapacityThresholds()) && (getBatteryVoltage() <= getBatteryWarningVoltage()))))
        TEXT_ATTRIBUTES_ADD_BLINK(*attr);
}

static void osdCrosshairsBounds(uint8_t *x, uint8_t *y, uint8_t *length)
{
    *x = 14 - 1; // Offset for 1 char to the left
    *y = 6;
    if (IS_DISPLAY_PAL) {
        ++(*y);
    }
    int size = 3;
    if (osdConfig()->crosshairs_style == OSD_CROSSHAIRS_STYLE_AIRCRAFT) {
        (*x)--;
        size = 5;
    }
    if (length) {
        *length = size;
    }
}

/**
 * Formats throttle position prefixed by its symbol. If autoThr
 * is true and the navigation system is controlling THR, it
 * uses the THR value applied by the system rather than the
 * input value received by the sticks.
 **/
static void osdFormatThrottlePosition(char *buff, bool autoThr, textAttributes_t *elemAttr)
{
    buff[0] = SYM_THR;
    buff[1] = SYM_THR1;
    int16_t thr = rcData[THROTTLE];
    if (autoThr && navigationIsControllingThrottle()) {
        buff[0] = SYM_AUTO_THR0;
        buff[1] = SYM_AUTO_THR1;
        thr = rcCommand[THROTTLE];
        if (isFixedWingAutoThrottleManuallyIncreased())
            TEXT_ATTRIBUTES_ADD_BLINK(*elemAttr);
    }
    tfp_sprintf(buff + 2, "%3d", (constrain(thr, PWM_RANGE_MIN, PWM_RANGE_MAX) - PWM_RANGE_MIN) * 100 / (PWM_RANGE_MAX - PWM_RANGE_MIN));
}

static inline int32_t osdGetAltitude(void)
{
#if defined(USE_NAV)
    return getEstimatedActualPosition(Z);
#elif defined(USE_BARO)
    return baro.alt;
#else
    return 0;
#endif
}

static uint8_t osdUpdateSidebar(osd_sidebar_scroll_e scroll, osd_sidebar_t *sidebar, timeMs_t currentTimeMs)
{
    // Scroll between SYM_AH_DECORATION_MIN and SYM_AH_DECORATION_MAX.
    // Zero scrolling should draw SYM_AH_DECORATION.
    uint8_t decoration = SYM_AH_DECORATION;
    int offset;
    int steps;
    switch (scroll) {
        case OSD_SIDEBAR_SCROLL_NONE:
            sidebar->arrow = OSD_SIDEBAR_ARROW_NONE;
            sidebar->offset = 0;
            return decoration;
        case OSD_SIDEBAR_SCROLL_ALTITUDE:
            // Move 1 char for every 20cm
            offset = osdGetAltitude();
            steps = offset / 20;
            break;
        case OSD_SIDEBAR_SCROLL_GROUND_SPEED:
#if defined(USE_GPS)
            offset = gpsSol.groundSpeed;
#else
            offset = 0;
#endif
            // Move 1 char for every 20 cm/s
            steps = offset / 20;
            break;
        case OSD_SIDEBAR_SCROLL_HOME_DISTANCE:
#if defined(USE_GPS)
            offset = GPS_distanceToHome;
#else
            offset = 0;
#endif
            // Move 1 char for every 5m
            steps = offset / 5;
            break;
    }
    if (offset) {
        decoration -= steps % SYM_AH_DECORATION_COUNT;
        if (decoration > SYM_AH_DECORATION_MAX) {
            decoration -= SYM_AH_DECORATION_COUNT;
        } else if (decoration < SYM_AH_DECORATION_MIN) {
            decoration += SYM_AH_DECORATION_COUNT;
        }
    }
    if (currentTimeMs - sidebar->updated > 100) {
        if (offset > sidebar->offset) {
            sidebar->arrow = OSD_SIDEBAR_ARROW_UP;
            sidebar->idle = 0;
        } else if (offset < sidebar->offset) {
            sidebar->arrow = OSD_SIDEBAR_ARROW_DOWN;
            sidebar->idle = 0;
        } else {
            if (sidebar->idle > 3) {
                sidebar->arrow = OSD_SIDEBAR_ARROW_NONE;
            } else {
                sidebar->idle++;
            }
        }
        sidebar->offset = offset;
        sidebar->updated = currentTimeMs;
    }
    return decoration;
}

static bool osdIsHeadingValid(void)
{
    return isImuHeadingValid();
}

static int16_t osdGetHeading(void)
{
    return attitude.values.yaw;
}

// Returns a heading angle in degrees normalized to [0, 360).
static int osdGetHeadingAngle(int angle)
{
    while (angle < 0) {
        angle += 360;
    }
    while (angle >= 360) {
        angle -= 360;
    }
    return angle;
}

#if defined(USE_GPS)

/* Draws a map with the given symbol in the center and given point of interest
 * defined by its distance in meters and direction in degrees.
 * referenceHeading indicates the up direction in the map, in degrees, while
 * referenceSym (if non-zero) is drawn at the upper right corner below a small
 * arrow to indicate the map reference to the user. The drawn argument is an
 * in-out used to store the last position where the craft was drawn to avoid
 * erasing all screen on each redraw.
 */
static void osdDrawMap(int referenceHeading, uint8_t referenceSym, uint8_t centerSym,
                       uint32_t poiDistance, int16_t poiDirection, uint8_t poiSymbol,
                       uint16_t *drawn, uint32_t *usedScale)
{
    // TODO: These need to be tested with several setups. We might
    // need to make them configurable.
    const int hMargin = 1;
    const int vMargin = 1;

    // TODO: Get this from the display driver?
    const int charWidth = 12;
    const int charHeight = 18;

    char buf[16];

    uint8_t minX = hMargin;
    uint8_t maxX = osdDisplayPort->cols - 1 - hMargin;
    uint8_t minY = vMargin;
    uint8_t maxY = osdDisplayPort->rows - 1 - vMargin;
    uint8_t midX = osdDisplayPort->cols / 2;
    uint8_t midY = osdDisplayPort->rows / 2;

    // Fixed marks
    if (referenceSym) {
        displayWriteChar(osdDisplayPort, maxX, minY, SYM_DIRECTION);
        displayWriteChar(osdDisplayPort, maxX, minY + 1, referenceSym);
    }
    displayWriteChar(osdDisplayPort, minX, maxY, SYM_SCALE);
    displayWriteChar(osdDisplayPort, midX, midY, centerSym);

    // First, erase the previous drawing.
    if (OSD_VISIBLE(*drawn)) {
        displayWriteChar(osdDisplayPort, OSD_X(*drawn), OSD_Y(*drawn), SYM_BLANK);
        *drawn = 0;
    }

    uint32_t initialScale;
    float scaleToUnit;
    int scaleUnitDivisor;
    char symUnscaled;
    char symScaled;
    int maxDecimals;
    const int scaleMultiplier = 2;
    // We try to reduce the scale when the POI will be around half the distance
    // between the center and the closers map edge, to avoid too much jumping
    const int scaleReductionMultiplier = MIN(midX - hMargin, midY - vMargin) / 2;

    switch (osdConfig()->units) {
        case OSD_UNIT_IMPERIAL:
            initialScale = 16; // 16m ~= 0.01miles
            scaleToUnit = 100 / 1609.3440f; // scale to 0.01mi for osdFormatCentiNumber()
            scaleUnitDivisor = 0;
            symUnscaled = SYM_MI;
            symScaled = SYM_MI;
            maxDecimals = 2;
            break;
        case OSD_UNIT_UK:
            FALLTHROUGH;
        case OSD_UNIT_METRIC:
            initialScale = 10; // 10m as initial scale
            scaleToUnit = 100; // scale to cm for osdFormatCentiNumber()
            scaleUnitDivisor = 1000; // Convert to km when scale gets bigger than 999m
            symUnscaled = SYM_M;
            symScaled = SYM_KM;
            maxDecimals = 0;
            break;
    }

    // Try to keep the same scale when getting closer until we draw over the center point
    uint32_t scale = initialScale;
    if (*usedScale) {
        scale = *usedScale;
        if (scale > initialScale && poiDistance < *usedScale * scaleReductionMultiplier) {
            scale /= scaleMultiplier;
        }
    }

    if (STATE(GPS_FIX)) {

        int directionToPoi = osdGetHeadingAngle(poiDirection - referenceHeading);
        float poiAngle = DEGREES_TO_RADIANS(directionToPoi);
        float poiSin = sin_approx(poiAngle);
        float poiCos = cos_approx(poiAngle);

        // Now start looking for a valid scale that lets us draw everything
        for (int ii = 0; ii < 50; ii++, scale *= scaleMultiplier) {
            // Calculate location of the aircraft in map
            int points = poiDistance / (float)(scale / charHeight);

            float pointsX = points * poiSin;
            int poiX = midX - roundf(pointsX / charWidth);
            if (poiX < minX || poiX > maxX) {
                continue;
            }

            float pointsY = points * poiCos;
            int poiY = midY + roundf(pointsY / charHeight);
            if (poiY < minY || poiY > maxY) {
                continue;
            }

            if (poiX == midX && poiY == midY) {
                // We're over the map center symbol, so we would be drawing
                // over it even if we increased the scale. Alternate between
                // drawing the center symbol or drawing the POI.
                if (centerSym != SYM_BLANK && OSD_ALTERNATING_CHOICES(1000, 2) == 0) {
                    break;
                }
            } else {

                uint8_t c;
                if (displayReadCharWithAttr(osdDisplayPort, poiX, poiY, &c, NULL) && c != SYM_BLANK) {
                    // Something else written here, increase scale. If the display doesn't support reading
                    // back characters, we assume there's nothing.
                    continue;
                }
            }

            // Draw the point on the map
            if (poiSymbol == SYM_ARROW_UP) {
                // Drawing aircraft, rotate
                int mapHeading = osdGetHeadingAngle(DECIDEGREES_TO_DEGREES(osdGetHeading()) - referenceHeading);
                poiSymbol += mapHeading * 2 / 45;
            }
            displayWriteChar(osdDisplayPort, poiX, poiY, poiSymbol);

            // Update saved location
            *drawn = OSD_POS(poiX, poiY) | OSD_VISIBLE_FLAG;
            break;
        }
    }

    // Draw the used scale
    bool scaled = osdFormatCentiNumber(buf, scale * scaleToUnit, scaleUnitDivisor, maxDecimals, 2, 3);
    buf[3] = scaled ? symScaled : symUnscaled;
    buf[4] = '\0';
    displayWrite(osdDisplayPort, minX + 1, maxY, buf);
    *usedScale = scale;
}

/* Draws a map with the home in the center and the craft moving around.
 * See osdDrawMap() for reference.
 */
static void osdDrawHomeMap(int referenceHeading, uint8_t referenceSym, uint16_t *drawn, uint32_t *usedScale)
{
    osdDrawMap(referenceHeading, referenceSym, SYM_HOME, GPS_distanceToHome, GPS_directionToHome, SYM_ARROW_UP, drawn, usedScale);
}

/* Draws a map with the aircraft in the center and the home moving around.
 * See osdDrawMap() for reference.
 */
static void osdDrawRadar(uint16_t *drawn, uint32_t *usedScale)
{
    int16_t reference = DECIDEGREES_TO_DEGREES(osdGetHeading());
    int16_t poiDirection = osdGetHeadingAngle(GPS_directionToHome + 180);
    osdDrawMap(reference, 0, SYM_ARROW_UP, GPS_distanceToHome, poiDirection, SYM_HOME, drawn, usedScale);
}

#endif

static void osdFormatPidControllerOutput(char *buff, const char *label, const pidController_t *pidController, uint8_t scale, bool showDecimal) {
    strcpy(buff, label);
    for (uint8_t i = strlen(label); i < 5; ++i) buff[i] = ' ';
    uint8_t decimals = showDecimal ? 1 : 0;
    osdFormatCentiNumber(buff + 5, pidController->proportional * scale, 0, decimals, 0, 4);
    buff[9] = ' ';
    osdFormatCentiNumber(buff + 10, pidController->integrator * scale, 0, decimals, 0, 4);
    buff[14] = ' ';
    osdFormatCentiNumber(buff + 15, pidController->derivative * scale, 0, decimals, 0, 4);
    buff[19] = ' ';
    osdFormatCentiNumber(buff + 20, pidController->output_constrained * scale, 0, decimals, 0, 4);
    buff[24] = '\0';
}

static void osdDisplayBatteryVoltage(uint8_t elemPosX, uint8_t elemPosY, uint16_t voltage, uint8_t digits, uint8_t decimals)
{
    char buff[6];
    textAttributes_t elemAttr = TEXT_ATTRIBUTES_NONE;

    osdFormatBatteryChargeSymbol(buff);
    buff[1] = '\0';
    osdUpdateBatteryCapacityOrVoltageTextAttributes(&elemAttr);
    displayWriteWithAttr(osdDisplayPort, elemPosX, elemPosY, buff, elemAttr);

    elemAttr = TEXT_ATTRIBUTES_NONE;
    osdFormatCentiNumber(buff, voltage, 0, decimals, 0, MIN(digits, 4));
    strcat(buff, "V");
    if ((getBatteryState() != BATTERY_NOT_PRESENT) && (getBatteryVoltage() <= getBatteryWarningVoltage()))
        TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
    displayWriteWithAttr(osdDisplayPort, elemPosX + 1, elemPosY, buff, elemAttr);
}

static void osdDisplayPIDValues(uint8_t elemPosX, uint8_t elemPosY, const char *str, const pid8_t *pid, adjustmentFunction_e adjFuncP, adjustmentFunction_e adjFuncI, adjustmentFunction_e adjFuncD)
{
    textAttributes_t elemAttr;
    char buff[4];

    displayWrite(osdDisplayPort, elemPosX, elemPosY, str);

    elemAttr = TEXT_ATTRIBUTES_NONE;
    tfp_sprintf(buff, "%3d", pid->P);
    if ((isAdjustmentFunctionSelected(adjFuncP)) || (((adjFuncP == ADJUSTMENT_ROLL_P) || (adjFuncP == ADJUSTMENT_PITCH_P)) && (isAdjustmentFunctionSelected(ADJUSTMENT_PITCH_ROLL_P))))
        TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
    displayWriteWithAttr(osdDisplayPort, elemPosX + 4, elemPosY, buff, elemAttr);

    elemAttr = TEXT_ATTRIBUTES_NONE;
    tfp_sprintf(buff, "%3d", pid->I);
    if ((isAdjustmentFunctionSelected(adjFuncI)) || (((adjFuncI == ADJUSTMENT_ROLL_I) || (adjFuncI == ADJUSTMENT_PITCH_I)) && (isAdjustmentFunctionSelected(ADJUSTMENT_PITCH_ROLL_I))))
        TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
    displayWriteWithAttr(osdDisplayPort, elemPosX + 8, elemPosY, buff, elemAttr);

    elemAttr = TEXT_ATTRIBUTES_NONE;
    tfp_sprintf(buff, "%3d", pid->D);
    if ((isAdjustmentFunctionSelected(adjFuncD)) || (((adjFuncD == ADJUSTMENT_ROLL_D) || (adjFuncD == ADJUSTMENT_PITCH_D)) && (isAdjustmentFunctionSelected(ADJUSTMENT_PITCH_ROLL_D))))
        TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
    displayWriteWithAttr(osdDisplayPort, elemPosX + 12, elemPosY, buff, elemAttr);
}

static void osdDisplayAdjustableDecimalValue(uint8_t elemPosX, uint8_t elemPosY, const char *str, const uint8_t valueOffset, const float value, const uint8_t valueLength, const uint8_t maxDecimals, adjustmentFunction_e adjFunc) {
    char buff[8];
    textAttributes_t elemAttr;
    displayWrite(osdDisplayPort, elemPosX, elemPosY, str);

    elemAttr = TEXT_ATTRIBUTES_NONE;
    osdFormatCentiNumber(buff, value * 100, 0, maxDecimals, 0, MIN(valueLength, 8));
    if (isAdjustmentFunctionSelected(adjFunc))
        TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
    displayWriteWithAttr(osdDisplayPort, elemPosX + strlen(str) + 1 + valueOffset, elemPosY, buff, elemAttr);
}

static bool osdDrawSingleElement(uint8_t item)
{
    uint16_t pos = osdConfig()->item_pos[currentLayout][item];
    if (!OSD_VISIBLE(pos)) {
        return false;
    }

    uint8_t elemPosX = OSD_X(pos);
    uint8_t elemPosY = OSD_Y(pos);
    textAttributes_t elemAttr = TEXT_ATTRIBUTES_NONE;
    char buff[32];

    switch (item) {
    case OSD_RSSI_VALUE:
        {
            uint16_t osdRssi = osdConvertRSSI();
            buff[0] = SYM_RSSI;
            tfp_sprintf(buff + 1, "%2d", osdRssi);
            if (osdRssi < osdConfig()->rssi_alarm) {
                TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
            }
            break;
        }

    case OSD_MAIN_BATT_VOLTAGE:
        osdDisplayBatteryVoltage(elemPosX, elemPosY, getBatteryRawVoltage(), 2 + osdConfig()->main_voltage_decimals, osdConfig()->main_voltage_decimals);
        return true;

    case OSD_SAG_COMPENSATED_MAIN_BATT_VOLTAGE:
        osdDisplayBatteryVoltage(elemPosX, elemPosY, getBatterySagCompensatedVoltage(), 2 + osdConfig()->main_voltage_decimals, osdConfig()->main_voltage_decimals);
        return true;

    case OSD_CURRENT_DRAW:
        buff[0] = SYM_AMP;
        osdFormatCentiNumber(buff + 1, getAmperage(), 0, 2, 0, 3);
        break;

    case OSD_MAH_DRAWN:
        buff[0] = SYM_MAH;
        tfp_sprintf(buff + 1, "%-4d", getMAhDrawn());
        osdUpdateBatteryCapacityOrVoltageTextAttributes(&elemAttr);
        break;

    case OSD_WH_DRAWN:
        buff[0] = SYM_WH;
        osdFormatCentiNumber(buff + 1, getMWhDrawn() / 10, 0, 2, 0, 3);
        osdUpdateBatteryCapacityOrVoltageTextAttributes(&elemAttr);
        break;

    case OSD_BATTERY_REMAINING_CAPACITY:
        buff[0] = (currentBatteryProfile->capacity.unit == BAT_CAPACITY_UNIT_MAH ? SYM_MAH : SYM_WH);

        if (currentBatteryProfile->capacity.value == 0)
            tfp_sprintf(buff + 1, "NA");
        else if (!batteryWasFullWhenPluggedIn())
            tfp_sprintf(buff + 1, "NF");
        else if (currentBatteryProfile->capacity.unit == BAT_CAPACITY_UNIT_MAH)
            tfp_sprintf(buff + 1, "%-4lu", getBatteryRemainingCapacity());
        else // currentBatteryProfile->capacity.unit == BAT_CAPACITY_UNIT_MWH
            osdFormatCentiNumber(buff + 1, getBatteryRemainingCapacity() / 10, 0, 2, 0, 3);

        if ((getBatteryState() != BATTERY_NOT_PRESENT) && batteryUsesCapacityThresholds() && (getBatteryRemainingCapacity() <= currentBatteryProfile->capacity.warning - currentBatteryProfile->capacity.critical))
            TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);

        break;

    case OSD_BATTERY_REMAINING_PERCENT:
        tfp_sprintf(buff, "%3d%%", calculateBatteryPercentage());
        osdUpdateBatteryCapacityOrVoltageTextAttributes(&elemAttr);
        break;

    case OSD_POWER_SUPPLY_IMPEDANCE:
        if (isPowerSupplyImpedanceValid())
            tfp_sprintf(buff, "%3d", getPowerSupplyImpedance());
        else
            strcpy(buff, "---");
        buff[3] = SYM_MILLIOHM;
        buff[4] = '\0';
        break;

#ifdef USE_GPS
    case OSD_GPS_SATS:
        buff[0] = SYM_SAT_L;
        buff[1] = SYM_SAT_R;
        tfp_sprintf(buff + 2, "%2d", gpsSol.numSat);
        if (!STATE(GPS_FIX)) {
            TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
        }
        break;

    case OSD_GPS_SPEED:
        osdFormatVelocityStr(buff, gpsSol.groundSpeed);
        break;

    case OSD_GPS_LAT:
        osdFormatCoordinate(buff, SYM_LAT, gpsSol.llh.lat);
        break;

    case OSD_GPS_LON:
        osdFormatCoordinate(buff, SYM_LON, gpsSol.llh.lon);
        break;

    case OSD_HOME_DIR:
        {
            // There are 16 orientations for the home direction arrow.
            // so we use 22.5deg per image, where the 1st image is used
            // for [349, 11], the 2nd for [12, 33], etc...
            int homeDirection = GPS_directionToHome - DECIDEGREES_TO_DEGREES(osdGetHeading());
            // Add 11 to the angle, so first character maps to [349, 11]
            int homeArrowDir = osdGetHeadingAngle(homeDirection + 11);
            unsigned arrowOffset = homeArrowDir * 2 / 45;
            buff[0] = SYM_ARROW_UP + arrowOffset;
            buff[1] = 0;
            break;
        }

    case OSD_HOME_HEADING_ERROR:
        {
            buff[0] = SYM_HOME;
            buff[1] = SYM_HEADING;

            if (isImuHeadingValid() && navigationPositionEstimateIsHealthy()) {
                int16_t h = lrintf(CENTIDEGREES_TO_DEGREES((float)wrap_18000(DEGREES_TO_CENTIDEGREES((int32_t)GPS_directionToHome) - DECIDEGREES_TO_CENTIDEGREES((int32_t)osdGetHeading()))));
                tfp_sprintf(buff + 2, "%4d", h);
            } else {
                strcpy(buff + 2, "----");
            }

            buff[6] = SYM_DEGREES;
            buff[7] = '\0';
            break;
        }

    case OSD_HOME_DIST:
        {
            buff[0] = SYM_HOME;
            osdFormatDistanceSymbol(&buff[1], GPS_distanceToHome * 100);
            uint16_t dist_alarm = osdConfig()->dist_alarm;
            if (dist_alarm > 0 && GPS_distanceToHome > dist_alarm) {
                TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
            }
        }
        break;

    case OSD_TRIP_DIST:
        buff[0] = SYM_TRIP_DIST;
        osdFormatDistanceSymbol(buff + 1, getTotalTravelDistance());
        break;

    case OSD_HEADING:
        {
            buff[0] = SYM_HEADING;
            if (osdIsHeadingValid()) {
                int16_t h = DECIDEGREES_TO_DEGREES(osdGetHeading());
                if (h < 0) {
                    h += 360;
                }
                tfp_sprintf(&buff[1], "%3d", h);
            } else {
                buff[1] = buff[2] = buff[3] = '-';
            }
            buff[4] = SYM_DEGREES;
            buff[5] = '\0';
            break;
        }

    case OSD_CRUISE_HEADING_ERROR:
        {
            if (ARMING_FLAG(ARMED) && !FLIGHT_MODE(NAV_CRUISE_MODE)) {
                displayWrite(osdDisplayPort, elemPosX, elemPosY, "     ");
                return true;
            }

            buff[0] = SYM_HEADING;

            if ((!ARMING_FLAG(ARMED)) || (FLIGHT_MODE(NAV_CRUISE_MODE) && isAdjustingPosition())) {
                buff[1] = buff[2] = buff[3] = '-';
            } else if (FLIGHT_MODE(NAV_CRUISE_MODE)) {
                int16_t herr = lrintf(CENTIDEGREES_TO_DEGREES((float)navigationGetHeadingError()));
                if (ABS(herr) > 99)
                    strcpy(buff + 1, ">99");
                else
                    tfp_sprintf(buff + 1, "%3d", herr);
            }

            buff[4] = SYM_DEGREES;
            buff[5] = '\0';
            break;
        }

    case OSD_CRUISE_HEADING_ADJUSTMENT:
        {
            int16_t heading_adjust = lrintf(CENTIDEGREES_TO_DEGREES((float)getCruiseHeadingAdjustment()));

            if (ARMING_FLAG(ARMED) && ((!FLIGHT_MODE(NAV_CRUISE_MODE)) || !(isAdjustingPosition() || isAdjustingHeading() || (heading_adjust != 0)))) {
                displayWrite(osdDisplayPort, elemPosX, elemPosY, "      ");
                return true;
            }

            buff[0] = SYM_HEADING;

            if (!ARMING_FLAG(ARMED)) {
                buff[1] = buff[2] = buff[3] = buff[4] = '-';
            } else if (FLIGHT_MODE(NAV_CRUISE_MODE)) {
                tfp_sprintf(buff + 1, "%4d", heading_adjust);
            }

            buff[5] = SYM_DEGREES;
            buff[6] = '\0';
            break;
        }

    case OSD_GPS_HDOP:
        {
            buff[0] = SYM_HDP_L;
            buff[1] = SYM_HDP_R;
            int32_t centiHDOP = 100 * gpsSol.hdop / HDOP_SCALE;
            osdFormatCentiNumber(&buff[2], centiHDOP, 0, 1, 0, 2);
            break;
        }

    case OSD_MAP_NORTH:
        {
            static uint16_t drawn = 0;
            static uint32_t scale = 0;
            osdDrawHomeMap(0, 'N', &drawn, &scale);
            return true;
        }
    case OSD_MAP_TAKEOFF:
        {
            static uint16_t drawn = 0;
            static uint32_t scale = 0;
            osdDrawHomeMap(CENTIDEGREES_TO_DEGREES(navigationGetHomeHeading()), 'T', &drawn, &scale);
            return true;
        }
    case OSD_RADAR:
        {
            static uint16_t drawn = 0;
            static uint32_t scale = 0;
            osdDrawRadar(&drawn, &scale);
            return true;
        }
#endif // GPS

    case OSD_ALTITUDE:
        {
            int32_t alt = osdGetAltitude();
            osdFormatAltitudeSymbol(buff, alt);
            uint16_t alt_alarm = osdConfig()->alt_alarm;
            uint16_t neg_alt_alarm = osdConfig()->neg_alt_alarm;
            if ((alt_alarm > 0 && CENTIMETERS_TO_METERS(alt) > alt_alarm) ||
                (neg_alt_alarm > 0 && alt < 0 && -CENTIMETERS_TO_METERS(alt) > neg_alt_alarm)) {

                TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
            }
            break;
        }

    case OSD_ONTIME:
        {
            osdFormatOnTime(buff);
            break;
        }

    case OSD_FLYTIME:
        {
            osdFormatFlyTime(buff, &elemAttr);
            break;
        }

    case OSD_ONTIME_FLYTIME:
        {
            if (ARMING_FLAG(ARMED)) {
                osdFormatFlyTime(buff, &elemAttr);
            } else {
                osdFormatOnTime(buff);
            }
            break;
        }

    case OSD_REMAINING_FLIGHT_TIME_BEFORE_RTH:
        {
            static timeUs_t updatedTimestamp = 0;
            /*static int32_t updatedTimeSeconds = 0;*/
            timeUs_t currentTimeUs = micros();
            static int32_t timeSeconds = -1;
            if (cmpTimeUs(currentTimeUs, updatedTimestamp) >= 1000000) {
                timeSeconds = calculateRemainingFlightTimeBeforeRTH(osdConfig()->estimations_wind_compensation);
                updatedTimestamp = currentTimeUs;
            }
            if ((!ARMING_FLAG(ARMED)) || (timeSeconds == -1)) {
                buff[0] = SYM_FLY_M;
                strcpy(buff + 1, "--:--");
                updatedTimestamp = 0;
            } else if (timeSeconds == -2) {
                // Wind is too strong to come back with cruise throttle
                buff[0] = SYM_FLY_M;
                buff[1] = buff[2] = buff[4] = buff[5] = SYM_WIND_HORIZONTAL;
                buff[3] = ':';
                buff[6] = '\0';
                TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
            } else {
                osdFormatTime(buff, timeSeconds, SYM_FLY_M, SYM_FLY_H);
                if (timeSeconds == 0)
                    TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
            }
        }
        break;

    case OSD_REMAINING_DISTANCE_BEFORE_RTH:;
        static timeUs_t updatedTimestamp = 0;
        timeUs_t currentTimeUs = micros();
        static int32_t distanceMeters = -1;
        if (cmpTimeUs(currentTimeUs, updatedTimestamp) >= 1000000) {
            distanceMeters = calculateRemainingDistanceBeforeRTH(osdConfig()->estimations_wind_compensation);
            updatedTimestamp = currentTimeUs;
        }
        buff[0] = SYM_TRIP_DIST;
        if ((!ARMING_FLAG(ARMED)) || (distanceMeters == -1)) {
            buff[1] = SYM_DIST_M;
            strcpy(buff + 2, "---");
        } else if (distanceMeters == -2) {
            // Wind is too strong to come back with cruise throttle
            buff[1] = SYM_DIST_M;
            buff[2] = buff[3] = buff[4] = SYM_WIND_HORIZONTAL;
            buff[5] = '\0';
            TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
        } else {
            osdFormatDistanceSymbol(buff + 1, distanceMeters * 100);
            if (distanceMeters == 0)
                TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
        }
        break;

    case OSD_FLYMODE:
        {
            char *p = "ACRO";

            if (isAirmodeActive()) {
                p = "AIR ";
            }

            if (FLIGHT_MODE(FAILSAFE_MODE)) {
                p = "!FS!";
            } else if (FLIGHT_MODE(MANUAL_MODE)) {
                p = "MANU";
            } else if (FLIGHT_MODE(NAV_RTH_MODE)) {
                p = "RTH ";
            } else if (FLIGHT_MODE(NAV_POSHOLD_MODE)) {
                if (FLIGHT_MODE(NAV_ALTHOLD_MODE)) {
                    // 3D HOLD
                    p = "HOLD";
                } else {
                    p = " PH ";
                }
            } else if (FLIGHT_MODE(NAV_CRUISE_MODE) && FLIGHT_MODE(NAV_ALTHOLD_MODE)) {
                p = "3CRS";
            } else if (FLIGHT_MODE(NAV_CRUISE_MODE)) {
                p = "CRS ";
            } else if (FLIGHT_MODE(NAV_ALTHOLD_MODE) && navigationRequiresAngleMode()) {
                // If navigationRequiresAngleMode() returns false when ALTHOLD is active,
                // it means it can be combined with ANGLE, HORIZON, ACRO, etc...
                // and its display is handled by OSD_MESSAGES rather than OSD_FLYMODE.
                p = " AH ";
            } else if (FLIGHT_MODE(NAV_WP_MODE)) {
                p = " WP ";
            } else if (FLIGHT_MODE(ANGLE_MODE)) {
                p = "ANGL";
            } else if (FLIGHT_MODE(HORIZON_MODE)) {
                p = "HOR ";
            }

            displayWrite(osdDisplayPort, elemPosX, elemPosY, p);
            return true;
        }

    case OSD_CRAFT_NAME:
        if (strlen(systemConfig()->name) == 0)
            strcpy(buff, "CRAFT_NAME");
        else {
            for (int i = 0; i < MAX_NAME_LENGTH; i++) {
                buff[i] = sl_toupper((unsigned char)systemConfig()->name[i]);
                if (systemConfig()->name[i] == 0)
                    break;
            }
        }
        break;

    case OSD_THROTTLE_POS:
    {
        osdFormatThrottlePosition(buff, false, NULL);
        break;
    }

#if defined(VTX) || defined(USE_VTX_COMMON)
    case OSD_VTX_CHANNEL:
#if defined(VTX)
        // FIXME: This doesn't actually work. It's for boards with
        // builtin VTX.
        tfp_sprintf(buff, "CH:%2d", current_vtx_channel % CHANNELS_PER_BAND + 1);
#else
        {
            uint8_t band = 0;
            uint8_t channel = 0;
            uint8_t powerIndex = 0;
            char bandChr = '-';
            const char *channelStr = "-";
            char powerChr = '-';
            vtxDevice_t *vtxDevice = vtxCommonDevice();
            if (vtxDevice) {
                if (vtxCommonGetBandAndChannel(vtxDevice, &band, &channel)) {
                    bandChr = vtx58BandLetter[band];
                    channelStr = vtx58ChannelNames[channel];
                }
                if (vtxCommonGetPowerIndex(vtxDevice, &powerIndex)) {
                    powerChr = '0' + powerIndex;
                }
            }
            tfp_sprintf(buff, "CH:%c%s:%c", bandChr, channelStr, powerChr);
        }
#endif
        break;
#endif // VTX || VTX_COMMON

    case OSD_CROSSHAIRS:
        osdCrosshairsBounds(&elemPosX, &elemPosY, NULL);
        switch ((osd_crosshairs_style_e)osdConfig()->crosshairs_style) {
            case OSD_CROSSHAIRS_STYLE_DEFAULT:
                buff[0] = SYM_AH_CENTER_LINE;
                buff[1] = SYM_AH_CENTER;
                buff[2] = SYM_AH_CENTER_LINE_RIGHT;
                buff[3] = '\0';
                break;
            case OSD_CROSSHAIRS_STYLE_AIRCRAFT:
                buff[0] = SYM_AH_CROSSHAIRS_AIRCRAFT0;
                buff[1] = SYM_AH_CROSSHAIRS_AIRCRAFT1;
                buff[2] = SYM_AH_CROSSHAIRS_AIRCRAFT2;
                buff[3] = SYM_AH_CROSSHAIRS_AIRCRAFT3;
                buff[4] = SYM_AH_CROSSHAIRS_AIRCRAFT4;
                buff[5] = '\0';
                break;
        }
        break;

    case OSD_ATTITUDE_ROLL:
        buff[0] = SYM_ROLL_LEVEL;
        if (ABS(attitude.values.roll) >= (osdConfig()->attitude_angle_decimals ? 1 : 10))
            buff[0] += (attitude.values.roll < 0 ? -1 : 1);
        osdFormatCentiNumber(buff + 1, ABS(attitude.values.roll) * 10, 0, osdConfig()->attitude_angle_decimals, 0, 2 + osdConfig()->attitude_angle_decimals);
        break;

    case OSD_ATTITUDE_PITCH:
        if (ABS(attitude.values.pitch) < (osdConfig()->attitude_angle_decimals ? 1 : 10))
            buff[0] = 'P';
        else if (attitude.values.pitch > 0)
            buff[0] = SYM_PITCH_DOWN;
        else if (attitude.values.pitch < 0)
            buff[0] = SYM_PITCH_UP;
        osdFormatCentiNumber(buff + 1, ABS(attitude.values.pitch) * 10, 0, osdConfig()->attitude_angle_decimals, 0, 2 + osdConfig()->attitude_angle_decimals);
        break;

    case OSD_ARTIFICIAL_HORIZON:
        {
            elemPosX = 14;
            elemPosY = 6 - 4; // Top center of the AH area
            // Erase only the positions we drew over. Avoids
            // thrashing the video driver buffer.
            // writtenY[x] contains the Y position that was
            // written to the OSD, counting from elemPosY.
            // A negative value indicates that the whole
            // column is blank.
            static int8_t writtenY[AH_SYMBOL_COUNT];

            bool crosshairsVisible;
            int crosshairsX, crosshairsY, crosshairsXEnd;

            int rollAngle = constrain(attitude.values.roll, -AH_MAX_ROLL, AH_MAX_ROLL);
            int pitchAngle = constrain(attitude.values.pitch, -AH_MAX_PITCH, AH_MAX_PITCH);

            if (osdConfig()->ahi_reverse_roll) {
                rollAngle = -rollAngle;
            }

            if (IS_DISPLAY_PAL) {
                ++elemPosY;
            }

            // Convert pitchAngle to y compensation value
            pitchAngle = ((pitchAngle * 25) / AH_MAX_PITCH) - 41; // 41 = 4 * 9 + 5
            crosshairsVisible = OSD_VISIBLE(osdConfig()->item_pos[currentLayout][OSD_CROSSHAIRS]);
            if (crosshairsVisible) {
                uint8_t cx, cy, cl;
                osdCrosshairsBounds(&cx, &cy, &cl);
                crosshairsX = cx - elemPosX;
                crosshairsY = cy - elemPosY;
                crosshairsXEnd = crosshairsX + cl - 1;
            }

            for (int x = -4; x <= 4; x++) {
                // Don't clear the whole area to save some time. Instead, clear
                // only the positions we previously wrote iff we're writing
                // at a different Y coordinate. The video buffer will take care
                // of ignoring the write if we're writing the same character
                // at the same Y coordinate.
                //
                // Note that this implementation leaves an untouched character
                // in the bottom center of the indicator, which allows positioning
                // the home directorion indicator there.
                const int y = (-rollAngle * x) / 64 - pitchAngle;
                int wx = x + 4; // map the -4 to the 1st element in the writtenY array
                int pwy = writtenY[wx]; // previously written Y at this X value
                int wy = (y / AH_SYMBOL_COUNT);
                // Check if we're overlapping with the crosshairs. Saves a few
                // trips to the video driver.
                bool overlaps = (crosshairsVisible &&
                            crosshairsY == wy &&
                            x >= crosshairsX && x <= crosshairsXEnd);

                if (y >= 0 && y <= 80 && !overlaps) {
                    if (pwy != -1 && pwy != wy) {
                        // Erase previous character at pwy rows below elemPosY
                        // iff we're writing at a different Y coordinate. Otherwise
                        // we just overwrite the previous one.
                        displayWriteChar(osdDisplayPort, elemPosX + x, elemPosY + pwy, SYM_BLANK);
                    }
                    uint8_t ch = SYM_AH_BAR9_0 + (y % AH_SYMBOL_COUNT);
                    displayWriteChar(osdDisplayPort, elemPosX + x, elemPosY + wy, ch);
                    writtenY[wx] = wy;
                } else {
                    if (pwy != -1) {
                        displayWriteChar(osdDisplayPort, elemPosX + x, elemPosY + pwy, SYM_BLANK);
                        writtenY[wx] = -1;
                    }
                }
            }

            osdDrawSingleElement(OSD_HORIZON_SIDEBARS);
            osdDrawSingleElement(OSD_CROSSHAIRS);

            return true;
        }

    case OSD_HORIZON_SIDEBARS:
        {
            elemPosX = 14;
            elemPosY = 6;

            if (IS_DISPLAY_PAL) {
                ++elemPosY;
            }

            static osd_sidebar_t left;
            static osd_sidebar_t right;

            timeMs_t currentTimeMs = millis();
            uint8_t leftDecoration = osdUpdateSidebar(osdConfig()->left_sidebar_scroll, &left, currentTimeMs);
            uint8_t rightDecoration = osdUpdateSidebar(osdConfig()->right_sidebar_scroll, &right, currentTimeMs);

            const int8_t hudwidth = AH_SIDEBAR_WIDTH_POS;
            const int8_t hudheight = AH_SIDEBAR_HEIGHT_POS;

            // Arrows
            if (osdConfig()->sidebar_scroll_arrows) {
                displayWriteChar(osdDisplayPort, elemPosX - hudwidth, elemPosY - hudheight - 1,
                    left.arrow == OSD_SIDEBAR_ARROW_UP ? SYM_AH_DECORATION_UP : SYM_BLANK);

                displayWriteChar(osdDisplayPort, elemPosX + hudwidth, elemPosY - hudheight - 1,
                    right.arrow == OSD_SIDEBAR_ARROW_UP ? SYM_AH_DECORATION_UP : SYM_BLANK);

                displayWriteChar(osdDisplayPort, elemPosX - hudwidth, elemPosY + hudheight + 1,
                    left.arrow == OSD_SIDEBAR_ARROW_DOWN ? SYM_AH_DECORATION_DOWN : SYM_BLANK);

                displayWriteChar(osdDisplayPort, elemPosX + hudwidth, elemPosY + hudheight + 1,
                    right.arrow == OSD_SIDEBAR_ARROW_DOWN ? SYM_AH_DECORATION_DOWN : SYM_BLANK);
            }

            // Draw AH sides
            for (int y = -hudheight; y <= hudheight; y++) {
                displayWriteChar(osdDisplayPort, elemPosX - hudwidth, elemPosY + y, leftDecoration);
                displayWriteChar(osdDisplayPort, elemPosX + hudwidth, elemPosY + y, rightDecoration);
            }

            // AH level indicators
            displayWriteChar(osdDisplayPort, elemPosX - hudwidth + 1, elemPosY, SYM_AH_LEFT);
            displayWriteChar(osdDisplayPort, elemPosX + hudwidth - 1, elemPosY, SYM_AH_RIGHT);

            return true;
        }

#if defined(USE_BARO) || defined(USE_GPS)
    case OSD_VARIO:
        {
            int16_t v = getEstimatedActualVelocity(Z) / 50; //50cm = 1 arrow
            uint8_t vchars[] = {SYM_BLANK, SYM_BLANK, SYM_BLANK, SYM_BLANK, SYM_BLANK};

            if (v >= 6)
                vchars[0] = SYM_VARIO_UP_2A;
            else if (v == 5)
                vchars[0] = SYM_VARIO_UP_1A;
            if (v >=4)
                vchars[1] = SYM_VARIO_UP_2A;
            else if (v == 3)
                vchars[1] = SYM_VARIO_UP_1A;
            if (v >=2)
                vchars[2] = SYM_VARIO_UP_2A;
            else if (v == 1)
                vchars[2] = SYM_VARIO_UP_1A;
            if (v <= -2)
                vchars[2] = SYM_VARIO_DOWN_2A;
            else if (v == -1)
                vchars[2] = SYM_VARIO_DOWN_1A;
            if (v <= -4)
                vchars[3] = SYM_VARIO_DOWN_2A;
            else if (v == -3)
                vchars[3] = SYM_VARIO_DOWN_1A;
            if (v <= -6)
                vchars[4] = SYM_VARIO_DOWN_2A;
            else if (v == -5)
                vchars[4] = SYM_VARIO_DOWN_1A;

            displayWriteChar(osdDisplayPort, elemPosX, elemPosY, vchars[0]);
            displayWriteChar(osdDisplayPort, elemPosX, elemPosY+1, vchars[1]);
            displayWriteChar(osdDisplayPort, elemPosX, elemPosY+2, vchars[2]);
            displayWriteChar(osdDisplayPort, elemPosX, elemPosY+3, vchars[3]);
            displayWriteChar(osdDisplayPort, elemPosX, elemPosY+4, vchars[4]);
            return true;
        }

    case OSD_VARIO_NUM:
        {
            int16_t value = getEstimatedActualVelocity(Z);
            char sym;
            switch ((osd_unit_e)osdConfig()->units) {
                case OSD_UNIT_IMPERIAL:
                    // Convert to centifeet/s
                    value = CENTIMETERS_TO_CENTIFEET(value);
                    sym = SYM_FTS;
                    break;
                case OSD_UNIT_UK:
                    FALLTHROUGH;
                case OSD_UNIT_METRIC:
                    // Already in cm/s
                    sym = SYM_MS;
                    break;
            }

            osdFormatCentiNumber(buff, value, 0, 1, 0, 3);
            buff[3] = sym;
            buff[4] = '\0';
            break;
        }
#endif

    case OSD_ROLL_PIDS:
        osdDisplayPIDValues(elemPosX, elemPosY, "ROL", &pidBank()->pid[PID_ROLL], ADJUSTMENT_ROLL_P, ADJUSTMENT_ROLL_I, ADJUSTMENT_ROLL_D);
        return true;

    case OSD_PITCH_PIDS:
        osdDisplayPIDValues(elemPosX, elemPosY, "PIT", &pidBank()->pid[PID_PITCH], ADJUSTMENT_PITCH_P, ADJUSTMENT_PITCH_I, ADJUSTMENT_PITCH_D);
        return true;

    case OSD_YAW_PIDS:
        osdDisplayPIDValues(elemPosX, elemPosY, "YAW", &pidBank()->pid[PID_YAW], ADJUSTMENT_YAW_P, ADJUSTMENT_YAW_I, ADJUSTMENT_YAW_D);
        return true;

    case OSD_LEVEL_PIDS:
        osdDisplayPIDValues(elemPosX, elemPosY, "LEV", &pidBank()->pid[PID_LEVEL], ADJUSTMENT_LEVEL_P, ADJUSTMENT_LEVEL_I, ADJUSTMENT_LEVEL_D);
        return true;

    case OSD_POS_XY_PIDS:
        osdDisplayPIDValues(elemPosX, elemPosY, "PXY", &pidBank()->pid[PID_POS_XY], ADJUSTMENT_POS_XY_P, ADJUSTMENT_POS_XY_I, ADJUSTMENT_POS_XY_D);
        return true;

    case OSD_POS_Z_PIDS:
        osdDisplayPIDValues(elemPosX, elemPosY, "PZ", &pidBank()->pid[PID_POS_Z], ADJUSTMENT_POS_Z_P, ADJUSTMENT_POS_Z_I, ADJUSTMENT_POS_Z_D);
        return true;

    case OSD_VEL_XY_PIDS:
        if (STATE(FIXED_WING))
            osdDisplayPIDValues(elemPosX, elemPosY, "VXY", &pidBank()->pid[PID_VEL_XY], ADJUSTMENT_VEL_XY_P, ADJUSTMENT_VEL_XY_I, ADJUSTMENT_VEL_XY_D);
        return true;

    case OSD_VEL_Z_PIDS:
        if (STATE(FIXED_WING))
            osdDisplayPIDValues(elemPosX, elemPosY, "VZ", &pidBank()->pid[PID_VEL_Z], ADJUSTMENT_VEL_Z_P, ADJUSTMENT_VEL_Z_I, ADJUSTMENT_VEL_Z_D);
        return true;

    case OSD_HEADING_P:
        osdDisplayAdjustableDecimalValue(elemPosX, elemPosY, "HP", 0, pidBank()->pid[PID_HEADING].P, 3, 0, ADJUSTMENT_HEADING_P);
        return true;

    case OSD_BOARD_ALIGN_ROLL:
        osdDisplayAdjustableDecimalValue(elemPosX, elemPosY, "AR", 0, DECIDEGREES_TO_DEGREES((float)boardAlignment()->rollDeciDegrees), 4, 1, ADJUSTMENT_ROLL_BOARD_ALIGNMENT);
        return true;

    case OSD_BOARD_ALIGN_PITCH:
        osdDisplayAdjustableDecimalValue(elemPosX, elemPosY, "AP", 0, DECIDEGREES_TO_DEGREES((float)boardAlignment()->pitchDeciDegrees), 4, 1, ADJUSTMENT_PITCH_BOARD_ALIGNMENT);
        return true;

    case OSD_RC_EXPO:
        osdDisplayAdjustableDecimalValue(elemPosX, elemPosY, "EXP", 0, currentControlRateProfile->stabilized.rcExpo8, 3, 0, ADJUSTMENT_RC_EXPO);
        return true;

    case OSD_RC_YAW_EXPO:
        osdDisplayAdjustableDecimalValue(elemPosX, elemPosY, "YEX", 0, currentControlRateProfile->stabilized.rcYawExpo8, 3, 0, ADJUSTMENT_RC_YAW_EXPO);
        return true;

    case OSD_THROTTLE_EXPO:
        osdDisplayAdjustableDecimalValue(elemPosX, elemPosY, "TEX", 0, currentControlRateProfile->throttle.rcExpo8, 3, 0, ADJUSTMENT_THROTTLE_EXPO);
        return true;

    case OSD_PITCH_RATE:
        displayWrite(osdDisplayPort, elemPosX, elemPosY, "SPR");

        elemAttr = TEXT_ATTRIBUTES_NONE;
        tfp_sprintf(buff, "%3d", currentControlRateProfile->stabilized.rates[FD_PITCH]);
        if (isAdjustmentFunctionSelected(ADJUSTMENT_PITCH_RATE) || isAdjustmentFunctionSelected(ADJUSTMENT_PITCH_ROLL_RATE))
            TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
        displayWriteWithAttr(osdDisplayPort, elemPosX + 4, elemPosY, buff, elemAttr);
        return true;

    case OSD_ROLL_RATE:
        displayWrite(osdDisplayPort, elemPosX, elemPosY, "SRR");

        elemAttr = TEXT_ATTRIBUTES_NONE;
        tfp_sprintf(buff, "%3d", currentControlRateProfile->stabilized.rates[FD_ROLL]);
        if (isAdjustmentFunctionSelected(ADJUSTMENT_ROLL_RATE) || isAdjustmentFunctionSelected(ADJUSTMENT_PITCH_ROLL_RATE))
            TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
        displayWriteWithAttr(osdDisplayPort, elemPosX + 4, elemPosY, buff, elemAttr);
        return true;

    case OSD_YAW_RATE:
        osdDisplayAdjustableDecimalValue(elemPosX, elemPosY, "SYR", 0, currentControlRateProfile->stabilized.rates[FD_YAW], 3, 0, ADJUSTMENT_YAW_RATE);
        return true;

    case OSD_MANUAL_RC_EXPO:
        osdDisplayAdjustableDecimalValue(elemPosX, elemPosY, "MEX", 0, currentControlRateProfile->manual.rcExpo8, 3, 0, ADJUSTMENT_MANUAL_RC_EXPO);
        return true;

    case OSD_MANUAL_RC_YAW_EXPO:
        osdDisplayAdjustableDecimalValue(elemPosX, elemPosY, "MYX", 0, currentControlRateProfile->manual.rcYawExpo8, 3, 0, ADJUSTMENT_MANUAL_RC_YAW_EXPO);
        return true;

    case OSD_MANUAL_PITCH_RATE:
        displayWrite(osdDisplayPort, elemPosX, elemPosY, "MPR");

        elemAttr = TEXT_ATTRIBUTES_NONE;
        tfp_sprintf(buff, "%3d", currentControlRateProfile->manual.rates[FD_PITCH]);
        if (isAdjustmentFunctionSelected(ADJUSTMENT_MANUAL_PITCH_RATE) || isAdjustmentFunctionSelected(ADJUSTMENT_MANUAL_PITCH_ROLL_RATE))
            TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
        displayWriteWithAttr(osdDisplayPort, elemPosX + 4, elemPosY, buff, elemAttr);
        return true;

    case OSD_MANUAL_ROLL_RATE:
        displayWrite(osdDisplayPort, elemPosX, elemPosY, "MRR");

        elemAttr = TEXT_ATTRIBUTES_NONE;
        tfp_sprintf(buff, "%3d", currentControlRateProfile->manual.rates[FD_ROLL]);
        if (isAdjustmentFunctionSelected(ADJUSTMENT_MANUAL_ROLL_RATE) || isAdjustmentFunctionSelected(ADJUSTMENT_MANUAL_PITCH_ROLL_RATE))
            TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
        displayWriteWithAttr(osdDisplayPort, elemPosX + 4, elemPosY, buff, elemAttr);
        return true;

    case OSD_MANUAL_YAW_RATE:
        osdDisplayAdjustableDecimalValue(elemPosX, elemPosY, "MYR", 0, currentControlRateProfile->stabilized.rates[FD_YAW], 3, 0, ADJUSTMENT_YAW_RATE);
        return true;

    case OSD_NAV_FW_CRUISE_THR:
        osdDisplayAdjustableDecimalValue(elemPosX, elemPosY, "CRS", 0, navConfig()->fw.cruise_throttle, 4, 0, ADJUSTMENT_NAV_FW_CRUISE_THR);
        return true;

    case OSD_NAV_FW_PITCH2THR:
        osdDisplayAdjustableDecimalValue(elemPosX, elemPosY, "P2T", 0, navConfig()->fw.pitch_to_throttle, 3, 0, ADJUSTMENT_NAV_FW_PITCH2THR);
        return true;

    case OSD_FW_MIN_THROTTLE_DOWN_PITCH_ANGLE:
        osdDisplayAdjustableDecimalValue(elemPosX, elemPosY, "0TP", 0, (float)mixerConfig()->fwMinThrottleDownPitchAngle / 10, 3, 1, ADJUSTMENT_FW_MIN_THROTTLE_DOWN_PITCH_ANGLE);
        return true;

    case OSD_FW_ALT_PID_OUTPUTS:
        {
            const navigationPIDControllers_t *nav_pids = getNavigationPIDControllers();
            osdFormatPidControllerOutput(buff, "PZO", &nav_pids->fw_alt, 10, true); // display requested pitch degrees
            break;
        }

    case OSD_FW_POS_PID_OUTPUTS:
        {
            const navigationPIDControllers_t *nav_pids = getNavigationPIDControllers(); // display requested roll degrees
            osdFormatPidControllerOutput(buff, "PXYO", &nav_pids->fw_nav, 1, true);
            break;
        }

    case OSD_MC_VEL_Z_PID_OUTPUTS:
        {
            const navigationPIDControllers_t *nav_pids = getNavigationPIDControllers();
            osdFormatPidControllerOutput(buff, "VZO", &nav_pids->vel[Z], 100, false); // display throttle adjustment µs
            break;
        }

    case OSD_MC_VEL_X_PID_OUTPUTS:
        {
            const navigationPIDControllers_t *nav_pids = getNavigationPIDControllers();
            osdFormatPidControllerOutput(buff, "VXO", &nav_pids->vel[X], 100, false); // display requested acceleration cm/s^2
            break;
        }

    case OSD_MC_VEL_Y_PID_OUTPUTS:
        {
            const navigationPIDControllers_t *nav_pids = getNavigationPIDControllers();
            osdFormatPidControllerOutput(buff, "VYO", &nav_pids->vel[Y], 100, false); // display requested acceleration cm/s^2
            break;
        }

    case OSD_MC_POS_XYZ_P_OUTPUTS:
        {
            const navigationPIDControllers_t *nav_pids = getNavigationPIDControllers();
            strcpy(buff, "POSO ");
            // display requested velocity cm/s
            tfp_sprintf(buff + 5, "%4d", lrintf(nav_pids->pos[X].output_constrained * 100));
            buff[9] = ' ';
            tfp_sprintf(buff + 10, "%4d", lrintf(nav_pids->pos[Y].output_constrained * 100));
            buff[14] = ' ';
            tfp_sprintf(buff + 15, "%4d", lrintf(nav_pids->pos[Z].output_constrained * 100));
            buff[19] = '\0';
            break;
        }

    case OSD_POWER:
        {
            buff[0] = SYM_WATT;
            osdFormatCentiNumber(buff + 1, getPower(), 0, 2, 0, 3);
            break;
        }

    case OSD_AIR_SPEED:
        {
        #ifdef USE_PITOT
            buff[0] = SYM_AIR;
            osdFormatVelocityStr(buff + 1, pitot.airSpeed);
        #else
            return false;
        #endif
            break;
        }

    case OSD_RTC_TIME:
        {
            // RTC not configured will show 00:00
            dateTime_t dateTime;
            rtcGetDateTimeLocal(&dateTime);
            buff[0] = SYM_CLOCK;
            tfp_sprintf(buff + 1, "%02u:%02u", dateTime.hours, dateTime.minutes);
            break;
        }

    case OSD_MESSAGES:
        {
            const char *message = NULL;
            if (ARMING_FLAG(ARMED)) {
                // Aircraft is armed. We might have up to 4
                // messages to show.
                const char *messages[4];
                unsigned messageCount = 0;
                if (FLIGHT_MODE(FAILSAFE_MODE)) {
                    // In FS mode while being armed too
                    const char *failsafePhaseMessage = osdFailsafePhaseMessage();
                    const char *failsafeInfoMessage = osdFailsafeInfoMessage();
                    const char *navStateFSMessage = navigationStateMessage();
                    if (failsafePhaseMessage) {
                        messages[messageCount++] = failsafePhaseMessage;
                    }
                    if (failsafeInfoMessage) {
                        messages[messageCount++] = failsafeInfoMessage;
                    }
                    if (navStateFSMessage) {
                        messages[messageCount++] = navStateFSMessage;
                    }
                    if (messageCount > 0) {
                        message = messages[OSD_ALTERNATING_CHOICES(1000, messageCount)];
                        if (message == failsafeInfoMessage) {
                            // failsafeInfoMessage is not useful for recovering
                            // a lost model, but might help avoiding a crash.
                            // Blink to grab user attention.
                            TEXT_ATTRIBUTES_ADD_BLINK(elemAttr);
                        }
                        // We're shoing either failsafePhaseMessage or
                        // navStateFSMessage. Don't BLINK here since
                        // having this text available might be crucial
                        // during a lost aircraft recovery and blinking
                        // will cause it to be missing from some frames.
                    }
                } else {
                    if (FLIGHT_MODE(NAV_RTH_MODE) || FLIGHT_MODE(NAV_WP_MODE)) {
                        const char *navStateMessage = navigationStateMessage();
                        if (navStateMessage) {
                            messages[messageCount++] = navStateMessage;
                        }
                    } else {
                        if (FLIGHT_MODE(NAV_ALTHOLD_MODE) && !navigationRequiresAngleMode()) {
                            // ALTHOLD might be enabled alongside ANGLE/HORIZON/ACRO
                            // when it doesn't require ANGLE mode (required only in FW
                            // right now). If if requires ANGLE, its display is handled
                            // by OSD_FLYMODE.
                            messages[messageCount++] = "(ALTITUDE HOLD)";
                        }
                        if (IS_RC_MODE_ACTIVE(BOXAUTOTRIM)) {
                            messages[messageCount++] = "(AUTOTRIM)";
                        }
                        if (IS_RC_MODE_ACTIVE(BOXAUTOTUNE)) {
                            messages[messageCount++] = "(AUTOTUNE)";
                        }
                        if (FLIGHT_MODE(HEADFREE_MODE)) {
                            messages[messageCount++] = "(HEADFREE)";
                        }
                    }
                    // Pick one of the available messages. Each message lasts
                    // a second.
                    if (messageCount > 0) {
                        message = messages[OSD_ALTERNATING_CHOICES(1000, messageCount)];
                    }
                }
            } else if (ARMING_FLAG(ARMING_DISABLED_ALL_FLAGS)) {
                // Check if we're unable to arm for some reason
                if (OSD_ALTERNATING_CHOICES(1000, 2) == 0) {
                    message = "UNABLE TO ARM";
                    TEXT_ATTRIBUTES_ADD_INVERTED(elemAttr);
                } else {
                    // Show the reason for not arming
                    message = osdArmingDisabledReasonMessage();
                }
            }
            osdFormatMessage(buff, sizeof(buff), message);
            break;
        }

    case OSD_MAIN_BATT_CELL_VOLTAGE:
        {
            osdDisplayBatteryVoltage(elemPosX, elemPosY, getBatteryRawAverageCellVoltage(), 3, 2);
            return true;
        }

    case OSD_MAIN_BATT_SAG_COMPENSATED_CELL_VOLTAGE:
        {
            osdDisplayBatteryVoltage(elemPosX, elemPosY, getBatterySagCompensatedAverageCellVoltage(), 3, 2);
            return true;
        }

    case OSD_THROTTLE_POS_AUTO_THR:
        {
            osdFormatThrottlePosition(buff, true, &elemAttr);
            break;
        }

    case OSD_HEADING_GRAPH:
        {
            static const uint8_t graph[] = {
                SYM_HEADING_LINE,
                SYM_HEADING_E,
                SYM_HEADING_LINE,
                SYM_HEADING_DIVIDED_LINE,
                SYM_HEADING_LINE,
                SYM_HEADING_S,
                SYM_HEADING_LINE,
                SYM_HEADING_DIVIDED_LINE,
                SYM_HEADING_LINE,
                SYM_HEADING_W,
                SYM_HEADING_LINE,
                SYM_HEADING_DIVIDED_LINE,
                SYM_HEADING_LINE,
                SYM_HEADING_N,
                SYM_HEADING_LINE,
                SYM_HEADING_DIVIDED_LINE,
                SYM_HEADING_LINE,
                SYM_HEADING_E,
                SYM_HEADING_LINE,
                SYM_HEADING_DIVIDED_LINE,
                SYM_HEADING_LINE,
                SYM_HEADING_S,
                SYM_HEADING_LINE,
                SYM_HEADING_DIVIDED_LINE,
                SYM_HEADING_LINE,
                SYM_HEADING_W,
                SYM_HEADING_LINE,
            };
            if (osdIsHeadingValid()) {
                int16_t h = DECIDEGREES_TO_DEGREES(osdGetHeading());
                if (h >= 180) {
                    h -= 360;
                }
                int hh = h * 4;
                hh = hh + 720 + 45;
                hh = hh / 90;
                memcpy_fn(buff, graph + hh + 1, 9);
            } else {
                buff[0] = buff[2] = buff[4] = buff[6] = buff[8] = SYM_HEADING_LINE;
                buff[1] = buff[3] = buff[5] = buff[7] = SYM_HEADING_DIVIDED_LINE;
            }
            buff[9] = '\0';
            break;
        }

    case OSD_EFFICIENCY_MAH_PER_KM:
        {
            // amperage is in centi amps, speed is in cms/s. We want
            // mah/km. Values over 999 are considered useless and
            // displayed as "---""
            static pt1Filter_t eFilterState;
            static timeUs_t efficiencyUpdated = 0;
            int32_t value = 0;
            timeUs_t currentTimeUs = micros();
            timeDelta_t efficiencyTimeDelta = cmpTimeUs(currentTimeUs, efficiencyUpdated);
            if (STATE(GPS_FIX) && gpsSol.groundSpeed > 0) {
                if (efficiencyTimeDelta >= EFFICIENCY_UPDATE_INTERVAL) {
                    value = pt1FilterApply4(&eFilterState, ((float)getAmperage() / gpsSol.groundSpeed) / 0.0036f,
                        1, efficiencyTimeDelta * 1e-6f);

                    efficiencyUpdated = currentTimeUs;
                } else {
                    value = eFilterState.state;
                }
            }
            if (value > 0 && value <= 999) {
                tfp_sprintf(buff, "%3d", value);
            } else {
                buff[0] = buff[1] = buff[2] = '-';
            }
            buff[3] = SYM_MAH_KM_0;
            buff[4] = SYM_MAH_KM_1;
            buff[5] = '\0';
            break;
        }

    case OSD_EFFICIENCY_WH_PER_KM:
        {
            // amperage is in centi amps, speed is in cms/s. We want
            // mWh/km. Values over 999Wh/km are considered useless and
            // displayed as "---""
            static pt1Filter_t eFilterState;
            static timeUs_t efficiencyUpdated = 0;
            int32_t value = 0;
            timeUs_t currentTimeUs = micros();
            timeDelta_t efficiencyTimeDelta = cmpTimeUs(currentTimeUs, efficiencyUpdated);
            if (STATE(GPS_FIX) && gpsSol.groundSpeed > 0) {
                if (efficiencyTimeDelta >= EFFICIENCY_UPDATE_INTERVAL) {
                    value = pt1FilterApply4(&eFilterState, ((float)getPower() / gpsSol.groundSpeed) / 0.0036f,
                        1, efficiencyTimeDelta * 1e-6f);

                    efficiencyUpdated = currentTimeUs;
                } else {
                    value = eFilterState.state;
                }
            }
            if (value > 0 && value <= 999999) {
                osdFormatCentiNumber(buff, value / 10, 0, 2, 0, 3);
            } else {
                buff[0] = buff[1] = buff[2] = '-';
            }
            buff[3] = SYM_WH_KM_0;
            buff[4] = SYM_WH_KM_1;
            buff[5] = '\0';
            break;
        }

    case OSD_DEBUG:
        {
            // Longest representable string is -32768, hence 6 characters
            tfp_sprintf(buff, "[0]=%6d [1]=%6d", debug[0], debug[1]);
            displayWrite(osdDisplayPort, elemPosX, elemPosY, buff);
            elemPosY++;
            tfp_sprintf(buff, "[2]=%6d [3]=%6d", debug[2], debug[3]);
            break;
        }

    case OSD_WIND_SPEED_HORIZONTAL:
#ifdef USE_WIND_ESTIMATOR
        {
            bool valid = isEstimatedWindSpeedValid();
            float horizontalWindSpeed;
            if (valid) {
                uint16_t angle;
                horizontalWindSpeed = getEstimatedHorizontalWindSpeed(&angle);
                int16_t windDirection = osdGetHeadingAngle((int)angle - DECIDEGREES_TO_DEGREES(attitude.values.yaw));
                buff[1] = SYM_DIRECTION + (windDirection * 2 / 90);
            } else {
                horizontalWindSpeed = 0;
                buff[1] = SYM_BLANK;
            }
            buff[0] = SYM_WIND_HORIZONTAL;
            osdFormatWindSpeedStr(buff + 2, horizontalWindSpeed, valid);
            break;
        }
#else
        return false;
#endif

    case OSD_WIND_SPEED_VERTICAL:
#ifdef USE_WIND_ESTIMATOR
        {
            buff[0] = SYM_WIND_VERTICAL;
            buff[1] = SYM_BLANK;
            bool valid = isEstimatedWindSpeedValid();
            float verticalWindSpeed;
            if (valid) {
                verticalWindSpeed = getEstimatedWindSpeed(Z);
                if (verticalWindSpeed < 0) {
                    buff[1] = SYM_AH_DECORATION_DOWN;
                    verticalWindSpeed = -verticalWindSpeed;
                } else if (verticalWindSpeed > 0) {
                    buff[1] = SYM_AH_DECORATION_UP;
                }
            } else {
                verticalWindSpeed = 0;
            }
            osdFormatWindSpeedStr(buff + 2, verticalWindSpeed, valid);
            break;
        }
#else
        return false;
#endif

    default:
        return false;
    }

    displayWriteWithAttr(osdDisplayPort, elemPosX, elemPosY, buff, elemAttr);
    return true;
}

static uint8_t osdIncElementIndex(uint8_t elementIndex)
{
    ++elementIndex;

    if (!sensors(SENSOR_ACC)) {
        if (elementIndex == OSD_CROSSHAIRS) {
            elementIndex = OSD_ONTIME;
        }
    }

    if (!feature(FEATURE_VBAT)) {
        if (elementIndex == OSD_SAG_COMPENSATED_MAIN_BATT_VOLTAGE) {
            elementIndex = OSD_LEVEL_PIDS;
        }
    }

    if (!feature(FEATURE_CURRENT_METER)) {
        if (elementIndex == OSD_CURRENT_DRAW) {
            elementIndex = OSD_GPS_SPEED;
        }
        if (elementIndex == OSD_EFFICIENCY_MAH_PER_KM) {
            elementIndex = OSD_TRIP_DIST;
        }
        if (elementIndex == OSD_REMAINING_FLIGHT_TIME_BEFORE_RTH) {
            elementIndex = OSD_HOME_HEADING_ERROR;
        }
        if (elementIndex == OSD_SAG_COMPENSATED_MAIN_BATT_VOLTAGE) {
            elementIndex = OSD_LEVEL_PIDS;
        }
    }

    if (!feature(FEATURE_GPS)) {
        if (elementIndex == OSD_GPS_SPEED) {
            elementIndex = OSD_ALTITUDE;
        }
        if (elementIndex == OSD_GPS_LON) {
            elementIndex = OSD_VARIO;
        }
        if (elementIndex == OSD_GPS_HDOP) {
            elementIndex = OSD_MAIN_BATT_CELL_VOLTAGE;
        }
        if (elementIndex == OSD_TRIP_DIST) {
            elementIndex = OSD_ATTITUDE_PITCH;
        }
        if (elementIndex == OSD_MAP_NORTH) {
            elementIndex = OSD_SAG_COMPENSATED_MAIN_BATT_VOLTAGE;
        }
    }

    if (elementIndex == OSD_ITEM_COUNT) {
        elementIndex = 0;
    }
    return elementIndex;
}

void osdDrawNextElement(void)
{
    static uint8_t elementIndex = 0;
    // Prevent infinite loop when no elements are enabled
    uint8_t index = elementIndex;
    do {
        elementIndex = osdIncElementIndex(elementIndex);
    } while(!osdDrawSingleElement(elementIndex) && index != elementIndex);
}

void pgResetFn_osdConfig(osdConfig_t *osdConfig)
{
    osdConfig->item_pos[0][OSD_ALTITUDE] = OSD_POS(1, 0) | OSD_VISIBLE_FLAG;
    osdConfig->item_pos[0][OSD_MAIN_BATT_VOLTAGE] = OSD_POS(12, 0) | OSD_VISIBLE_FLAG;
    osdConfig->item_pos[0][OSD_SAG_COMPENSATED_MAIN_BATT_VOLTAGE] = OSD_POS(12, 1);

    osdConfig->item_pos[0][OSD_RSSI_VALUE] = OSD_POS(23, 0) | OSD_VISIBLE_FLAG;
    //line 2
    osdConfig->item_pos[0][OSD_HOME_DIST] = OSD_POS(1, 1);
    osdConfig->item_pos[0][OSD_TRIP_DIST] = OSD_POS(1, 2);
    osdConfig->item_pos[0][OSD_MAIN_BATT_CELL_VOLTAGE] = OSD_POS(12, 1);
    osdConfig->item_pos[0][OSD_MAIN_BATT_SAG_COMPENSATED_CELL_VOLTAGE] = OSD_POS(12, 1);
    osdConfig->item_pos[0][OSD_GPS_SPEED] = OSD_POS(23, 1);

    osdConfig->item_pos[0][OSD_THROTTLE_POS] = OSD_POS(1, 2) | OSD_VISIBLE_FLAG;
    osdConfig->item_pos[0][OSD_THROTTLE_POS_AUTO_THR] = OSD_POS(6, 2);
    osdConfig->item_pos[0][OSD_HEADING] = OSD_POS(12, 2);
    osdConfig->item_pos[0][OSD_CRUISE_HEADING_ERROR] = OSD_POS(12, 2);
    osdConfig->item_pos[0][OSD_CRUISE_HEADING_ADJUSTMENT] = OSD_POS(12, 2);
    osdConfig->item_pos[0][OSD_HEADING_GRAPH] = OSD_POS(18, 2);
    osdConfig->item_pos[0][OSD_CURRENT_DRAW] = OSD_POS(1, 3) | OSD_VISIBLE_FLAG;
    osdConfig->item_pos[0][OSD_MAH_DRAWN] = OSD_POS(1, 4) | OSD_VISIBLE_FLAG;
    osdConfig->item_pos[0][OSD_WH_DRAWN] = OSD_POS(1, 5);
    osdConfig->item_pos[0][OSD_BATTERY_REMAINING_CAPACITY] = OSD_POS(1, 6);
    osdConfig->item_pos[0][OSD_BATTERY_REMAINING_PERCENT] = OSD_POS(1, 7);
    osdConfig->item_pos[0][OSD_POWER_SUPPLY_IMPEDANCE] = OSD_POS(1, 8);

    osdConfig->item_pos[0][OSD_EFFICIENCY_MAH_PER_KM] = OSD_POS(1, 5);
    osdConfig->item_pos[0][OSD_EFFICIENCY_WH_PER_KM] = OSD_POS(1, 5);

    osdConfig->item_pos[0][OSD_ATTITUDE_ROLL] = OSD_POS(1, 7);
    osdConfig->item_pos[0][OSD_ATTITUDE_PITCH] = OSD_POS(1, 8);

    // avoid OSD_VARIO under OSD_CROSSHAIRS
    osdConfig->item_pos[0][OSD_VARIO] = OSD_POS(23, 5);
    // OSD_VARIO_NUM at the right of OSD_VARIO
    osdConfig->item_pos[0][OSD_VARIO_NUM] = OSD_POS(24, 7);
    osdConfig->item_pos[0][OSD_HOME_DIR] = OSD_POS(14, 11);
    osdConfig->item_pos[0][OSD_ARTIFICIAL_HORIZON] = OSD_POS(8, 6) | OSD_VISIBLE_FLAG;
    osdConfig->item_pos[0][OSD_HORIZON_SIDEBARS] = OSD_POS(8, 6) | OSD_VISIBLE_FLAG;

    osdConfig->item_pos[0][OSD_CRAFT_NAME] = OSD_POS(20, 2);
    osdConfig->item_pos[0][OSD_VTX_CHANNEL] = OSD_POS(8, 6);

    osdConfig->item_pos[0][OSD_ONTIME] = OSD_POS(23, 8);
    osdConfig->item_pos[0][OSD_FLYTIME] = OSD_POS(23, 9);
    osdConfig->item_pos[0][OSD_ONTIME_FLYTIME] = OSD_POS(23, 11) | OSD_VISIBLE_FLAG;
    osdConfig->item_pos[0][OSD_RTC_TIME] = OSD_POS(23, 12);
    osdConfig->item_pos[0][OSD_REMAINING_FLIGHT_TIME_BEFORE_RTH] = OSD_POS(23, 7);
    osdConfig->item_pos[0][OSD_REMAINING_DISTANCE_BEFORE_RTH] = OSD_POS(23, 6);

    osdConfig->item_pos[0][OSD_GPS_SATS] = OSD_POS(0, 11) | OSD_VISIBLE_FLAG;
    osdConfig->item_pos[0][OSD_GPS_HDOP] = OSD_POS(0, 10);

    osdConfig->item_pos[0][OSD_GPS_LAT] = OSD_POS(0, 12);
    osdConfig->item_pos[0][OSD_FLYMODE] = OSD_POS(12, 12) | OSD_VISIBLE_FLAG;
    osdConfig->item_pos[0][OSD_GPS_LON] = OSD_POS(18, 12);

    osdConfig->item_pos[0][OSD_ROLL_PIDS] = OSD_POS(2, 10);
    osdConfig->item_pos[0][OSD_PITCH_PIDS] = OSD_POS(2, 11);
    osdConfig->item_pos[0][OSD_YAW_PIDS] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_LEVEL_PIDS] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_POS_XY_PIDS] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_POS_Z_PIDS] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_VEL_XY_PIDS] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_VEL_Z_PIDS] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_HEADING_P] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_BOARD_ALIGN_ROLL] = OSD_POS(2, 10);
    osdConfig->item_pos[0][OSD_BOARD_ALIGN_PITCH] = OSD_POS(2, 11);
    osdConfig->item_pos[0][OSD_RC_EXPO] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_RC_YAW_EXPO] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_THROTTLE_EXPO] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_PITCH_RATE] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_ROLL_RATE] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_YAW_RATE] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_MANUAL_RC_EXPO] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_MANUAL_RC_YAW_EXPO] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_MANUAL_PITCH_RATE] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_MANUAL_ROLL_RATE] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_MANUAL_YAW_RATE] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_NAV_FW_CRUISE_THR] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_NAV_FW_PITCH2THR] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_FW_MIN_THROTTLE_DOWN_PITCH_ANGLE] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_FW_ALT_PID_OUTPUTS] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_FW_POS_PID_OUTPUTS] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_MC_VEL_X_PID_OUTPUTS] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_MC_VEL_Y_PID_OUTPUTS] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_MC_VEL_Z_PID_OUTPUTS] = OSD_POS(2, 12);
    osdConfig->item_pos[0][OSD_MC_POS_XYZ_P_OUTPUTS] = OSD_POS(2, 12);

    osdConfig->item_pos[0][OSD_POWER] = OSD_POS(15, 1);

    osdConfig->item_pos[0][OSD_AIR_SPEED] = OSD_POS(3, 5);
    osdConfig->item_pos[0][OSD_WIND_SPEED_HORIZONTAL] = OSD_POS(3, 6);
    osdConfig->item_pos[0][OSD_WIND_SPEED_VERTICAL] = OSD_POS(3, 7);

    // Under OSD_FLYMODE. TODO: Might not be visible on NTSC?
    osdConfig->item_pos[0][OSD_MESSAGES] = OSD_POS(1, 13) | OSD_VISIBLE_FLAG;

    for (unsigned ii = 1; ii < OSD_LAYOUT_COUNT; ii++) {
        for (unsigned jj = 0; jj < ARRAYLEN(osdConfig->item_pos[0]); jj++) {
             osdConfig->item_pos[ii][jj] = osdConfig->item_pos[0][jj] & ~OSD_VISIBLE_FLAG;
        }
    }

    osdConfig->rssi_alarm = 20;
    osdConfig->time_alarm = 10;
    osdConfig->alt_alarm = 100;
    osdConfig->dist_alarm = 1000;
    osdConfig->neg_alt_alarm = 5;

    osdConfig->video_system = VIDEO_SYSTEM_AUTO;

    osdConfig->ahi_reverse_roll = 0;
    osdConfig->crosshairs_style = OSD_CROSSHAIRS_STYLE_DEFAULT;
    osdConfig->left_sidebar_scroll = OSD_SIDEBAR_SCROLL_NONE;
    osdConfig->right_sidebar_scroll = OSD_SIDEBAR_SCROLL_NONE;
    osdConfig->sidebar_scroll_arrows = 0;

    osdConfig->units = OSD_UNIT_METRIC;
    osdConfig->main_voltage_decimals = 1;
    osdConfig->attitude_angle_decimals = 0;

    osdConfig->estimations_wind_compensation = true;
}

static void osdSetNextRefreshIn(uint32_t timeMs) {
    resumeRefreshAt = micros() + timeMs * 1000;
    refreshWaitForResumeCmdRelease = true;
}

void osdInit(displayPort_t *osdDisplayPortToUse)
{
    if (!osdDisplayPortToUse)
        return;

    BUILD_BUG_ON(OSD_POS_MAX != OSD_POS(31,31));

    osdDisplayPort = osdDisplayPortToUse;

#ifdef USE_CMS
    cmsDisplayPortRegister(osdDisplayPort);
#endif

    armState = ARMING_FLAG(ARMED);

    displayClearScreen(osdDisplayPort);

    uint8_t y = 4;
    char string_buffer[30];
    tfp_sprintf(string_buffer, "INAV VERSION: %s", FC_VERSION_STRING);
    displayWrite(osdDisplayPort, 5, y++, string_buffer);
#ifdef USE_CMS
    displayWrite(osdDisplayPort, 7, y++,  CMS_STARTUP_HELP_TEXT1);
    displayWrite(osdDisplayPort, 11, y++, CMS_STARTUP_HELP_TEXT2);
    displayWrite(osdDisplayPort, 11, y++, CMS_STARTUP_HELP_TEXT3);
#endif

#ifdef USE_STATS
#define STATS_LABEL_X_POS 4
#define STATS_VALUE_X_POS 24
    if (statsConfig()->stats_enabled) {
        displayWrite(osdDisplayPort, STATS_LABEL_X_POS, ++y, "ODOMETER:");
        if (osdConfig()->units == OSD_UNIT_IMPERIAL) {
            tfp_sprintf(string_buffer, "%5d", statsConfig()->stats_total_dist / METERS_PER_MILE);
            string_buffer[5] = SYM_MI;
        } else {
            tfp_sprintf(string_buffer, "%5d", statsConfig()->stats_total_dist / METERS_PER_KILOMETER);
            string_buffer[5] = SYM_KM;
        }
        string_buffer[6] = '\0';
        displayWrite(osdDisplayPort, STATS_VALUE_X_POS-5, y,  string_buffer);

        displayWrite(osdDisplayPort, STATS_LABEL_X_POS, ++y, "TOTAL TIME:");
        uint32_t tot_mins = statsConfig()->stats_total_time / 60;
        tfp_sprintf(string_buffer, "%2d:%02dHM", tot_mins / 60, tot_mins % 60);
        displayWrite(osdDisplayPort, STATS_VALUE_X_POS-5, y,  string_buffer);

#ifdef USE_ADC
        if (feature(FEATURE_VBAT) && feature(FEATURE_CURRENT_METER)) {
            displayWrite(osdDisplayPort, STATS_LABEL_X_POS, ++y, "TOTAL ENERGY:");
            osdFormatCentiNumber(string_buffer, statsConfig()->stats_total_energy / 10, 0, 2, 0, 4);
            strcat(string_buffer, "\xAB"); // SYM_WH
            displayWrite(osdDisplayPort, STATS_VALUE_X_POS-4, y,  string_buffer);

            displayWrite(osdDisplayPort, STATS_LABEL_X_POS, ++y, "AVG EFFICIENCY:");
            if (statsConfig()->stats_total_dist) {
                uint32_t avg_efficiency = statsConfig()->stats_total_energy / (statsConfig()->stats_total_dist / METERS_PER_KILOMETER); // mWh/km
                osdFormatCentiNumber(string_buffer, avg_efficiency / 10, 0, 2, 0, 3);
            } else
                strcpy(string_buffer, "---");
            string_buffer[3] = SYM_WH_KM_0;
            string_buffer[4] = SYM_WH_KM_1;
            string_buffer[5] = '\0';
            displayWrite(osdDisplayPort, STATS_VALUE_X_POS-3, y,  string_buffer);
        }
#endif // USE_ADC
    }
#endif

    displayResync(osdDisplayPort);
    osdSetNextRefreshIn(SPLASH_SCREEN_DISPLAY_TIME);
}

static void osdResetStats(void)
{
    stats.max_current = 0;
    stats.max_power = 0;
    stats.max_speed = 0;
    stats.min_voltage = 5000;
    stats.min_rssi = 99;
    stats.max_altitude = 0;
}

static void osdUpdateStats(void)
{
    int16_t value;

    if (feature(FEATURE_GPS)) {
        value = gpsSol.groundSpeed;
        if (stats.max_speed < value)
            stats.max_speed = value;

        if (stats.max_distance < GPS_distanceToHome)
            stats.max_distance = GPS_distanceToHome;
    }

    value = getBatteryVoltage();
    if (stats.min_voltage > value)
        stats.min_voltage = value;

    value = abs(getAmperage() / 100);
    if (stats.max_current < value)
        stats.max_current = value;

    value = abs(getPower() / 100);
    if (stats.max_power < value)
        stats.max_power = value;

    value = osdConvertRSSI();
    if (stats.min_rssi > value)
        stats.min_rssi = value;

    stats.max_altitude = MAX(stats.max_altitude, osdGetAltitude());
}

/* Attention: NTSC screen only has 12 fully visible lines - it is FULL now! */
static void osdShowStats(void)
{
    const char * disarmReasonStr[DISARM_REASON_COUNT] = { "UNKNOWN", "TIMEOUT", "STICKS", "SWITCH", "SWITCH", "KILLSW", "FAILSAFE", "NAV SYS" };
    uint8_t top = 1;    /* first fully visible line */
    const uint8_t statNameX = 1;
    const uint8_t statValuesX = 20;
    char buff[10];

    displayClearScreen(osdDisplayPort);
    if (IS_DISPLAY_PAL)
        displayWrite(osdDisplayPort, statNameX, top++, "  --- STATS ---");

    if (STATE(GPS_FIX)) {
        displayWrite(osdDisplayPort, statNameX, top, "MAX SPEED        :");
        osdFormatVelocityStr(buff, stats.max_speed);
        displayWrite(osdDisplayPort, statValuesX, top++, buff);

        displayWrite(osdDisplayPort, statNameX, top, "MAX DISTANCE     :");
        osdFormatDistanceStr(buff, stats.max_distance*100);
        displayWrite(osdDisplayPort, statValuesX, top++, buff);

        displayWrite(osdDisplayPort, statNameX, top, "TRAVELED DISTANCE:");
        osdFormatDistanceStr(buff, getTotalTravelDistance());
        displayWrite(osdDisplayPort, statValuesX, top++, buff);
    }

    displayWrite(osdDisplayPort, statNameX, top, "MAX ALTITUDE     :");
    osdFormatAltitudeStr(buff, stats.max_altitude);
    displayWrite(osdDisplayPort, statValuesX, top++, buff);

    displayWrite(osdDisplayPort, statNameX, top, "MIN BATTERY VOLT :");
    osdFormatCentiNumber(buff, stats.min_voltage, 0, osdConfig()->main_voltage_decimals, 0, osdConfig()->main_voltage_decimals + 2);
    strcat(buff, "V");
    displayWrite(osdDisplayPort, statValuesX, top++, buff);

    displayWrite(osdDisplayPort, statNameX, top, "MIN RSSI         :");
    itoa(stats.min_rssi, buff, 10);
    strcat(buff, "%");
    displayWrite(osdDisplayPort, statValuesX, top++, buff);

    if (feature(FEATURE_CURRENT_METER)) {
        displayWrite(osdDisplayPort, statNameX, top, "MAX CURRENT      :");
        itoa(stats.max_current, buff, 10);
        strcat(buff, "A");
        displayWrite(osdDisplayPort, statValuesX, top++, buff);

        displayWrite(osdDisplayPort, statNameX, top, "MAX POWER        :");
        itoa(stats.max_power, buff, 10);
        strcat(buff, "W");
        displayWrite(osdDisplayPort, statValuesX, top++, buff);

        if (osdConfig()->stats_energy_unit == OSD_STATS_ENERGY_UNIT_MAH) {
            displayWrite(osdDisplayPort, statNameX, top, "USED MAH         :");
            tfp_sprintf(buff, "%d%c", getMAhDrawn(), SYM_MAH);
        } else {
            displayWrite(osdDisplayPort, statNameX, top, "USED WH          :");
            osdFormatCentiNumber(buff, getMWhDrawn() / 10, 0, 2, 0, 3);
            strcat(buff, "\xAB"); // SYM_WH
        }
        displayWrite(osdDisplayPort, statValuesX, top++, buff);

        int32_t totalDistance = getTotalTravelDistance();
        if (totalDistance > 0) {
            displayWrite(osdDisplayPort, statNameX, top, "AVG EFFICIENCY   :");
            if (osdConfig()->stats_energy_unit == OSD_STATS_ENERGY_UNIT_MAH)
                tfp_sprintf(buff, "%d%c%c", getMAhDrawn() * 100000 / totalDistance,
                    SYM_MAH_KM_0, SYM_MAH_KM_1);
            else {
                osdFormatCentiNumber(buff, getMWhDrawn() * 10000 / totalDistance, 0, 2, 0, 3);
                buff[3] = SYM_WH_KM_0;
                buff[4] = SYM_WH_KM_1;
                buff[5] = '\0';
            }
            displayWrite(osdDisplayPort, statValuesX, top++, buff);
        }
    }

    displayWrite(osdDisplayPort, statNameX, top, "FLY TIME         :");
    uint16_t flySeconds = getFlightTime();
    uint16_t flyMinutes = flySeconds / 60;
    flySeconds %= 60;
    uint16_t flyHours = flyMinutes / 60;
    flyMinutes %= 60;
    tfp_sprintf(buff, "%02u:%02u:%02u", flyHours, flyMinutes, flySeconds);
    displayWrite(osdDisplayPort, statValuesX, top++, buff);

    displayWrite(osdDisplayPort, statNameX, top, "DISARMED BY      :");
    displayWrite(osdDisplayPort, statValuesX, top++, disarmReasonStr[getDisarmReason()]);
}

// called when motors armed
static void osdShowArmed(void)
{
    dateTime_t dt;
    char buf[MAX(32, FORMATTED_DATE_TIME_BUFSIZE)];
    char *date;
    char *time;
    // We need 6 visible rows
    uint8_t y = MIN((osdDisplayPort->rows / 2) - 1, osdDisplayPort->rows - 6 - 1);

    displayClearScreen(osdDisplayPort);
    displayWrite(osdDisplayPort, 12, y, "ARMED");
    y += 2;

    if (STATE(GPS_FIX)) {
        osdFormatCoordinate(buf, SYM_LAT, GPS_home.lat);
        displayWrite(osdDisplayPort, (osdDisplayPort->cols - strlen(buf)) / 2, y, buf);
        osdFormatCoordinate(buf, SYM_LON, gpsSol.llh.lon);
        displayWrite(osdDisplayPort, (osdDisplayPort->cols - strlen(buf)) / 2, y + 1, buf);
        y += 3;
    }

    if (rtcGetDateTime(&dt)) {
        dateTimeFormatLocal(buf, &dt);
        dateTimeSplitFormatted(buf, &date, &time);

        displayWrite(osdDisplayPort, (osdDisplayPort->cols - strlen(date)) / 2, y, date);
        displayWrite(osdDisplayPort, (osdDisplayPort->cols - strlen(time)) / 2, y + 1, time);
    }
}

static void osdRefresh(timeUs_t currentTimeUs)
{
    if (IS_RC_MODE_ACTIVE(BOXOSD) && (!cmsInMenu)) {
      displayClearScreen(osdDisplayPort);
      armState = ARMING_FLAG(ARMED);
      return;
    }

    // detect arm/disarm
    if (armState != ARMING_FLAG(ARMED)) {
        if (ARMING_FLAG(ARMED)) {
            osdResetStats();
            osdShowArmed(); // reset statistic etc
            osdSetNextRefreshIn(ARMED_SCREEN_DISPLAY_TIME);
        } else {
            osdShowStats(); // show statistic
            osdSetNextRefreshIn(STATS_SCREEN_DISPLAY_TIME);
        }

        armState = ARMING_FLAG(ARMED);
    }

    if (resumeRefreshAt) {
        // If we already reached he time for the next refresh,
        // or THR is high or PITCH is high, resume refreshing.
        // Clear the screen first to erase other elements which
        // might have been drawn while the OSD wasn't refreshing.

        if (!DELAYED_REFRESH_RESUME_COMMAND)
            refreshWaitForResumeCmdRelease = false;

        if ((currentTimeUs > resumeRefreshAt) || ((!refreshWaitForResumeCmdRelease) && DELAYED_REFRESH_RESUME_COMMAND)) {
            displayClearScreen(osdDisplayPort);
            resumeRefreshAt = 0;
        } else {
            displayHeartbeat(osdDisplayPort);
        }
        return;
    }

#ifdef USE_CMS
    if (!displayIsGrabbed(osdDisplayPort)) {
        if (fullRedraw) {
            displayClearScreen(osdDisplayPort);
            fullRedraw = false;
        }
        osdDrawNextElement();
        displayHeartbeat(osdDisplayPort);
#ifdef OSD_CALLS_CMS
    } else {
        cmsUpdate(currentTimeUs);
#endif
    }
#endif
}

/*
 * Called periodically by the scheduler
 */
void osdUpdate(timeUs_t currentTimeUs)
{
    static uint32_t counter = 0;

    // don't touch buffers if DMA transaction is in progress
    if (displayIsTransferInProgress(osdDisplayPort)) {
        return;
    }

#if defined(OSD_ALTERNATE_LAYOUT_COUNT) && OSD_ALTERNATE_LAYOUT_COUNT > 0
    // Check if the layout has changed. Higher numbered
    // boxes take priority.
    unsigned activeLayout;
    if (layoutOverride >= 0) {
        activeLayout = layoutOverride;
    } else {
#if OSD_ALTERNATE_LAYOUT_COUNT > 2
        if (IS_RC_MODE_ACTIVE(BOXOSDALT3))
            activeLayout = 3;
        else
#endif
#if OSD_ALTERNATE_LAYOUT_COUNT > 1
        if (IS_RC_MODE_ACTIVE(BOXOSDALT2))
            activeLayout = 2;
        else
#endif
        if (IS_RC_MODE_ACTIVE(BOXOSDALT1))
            activeLayout = 1;
        else
            activeLayout = 0;
    }
    if (currentLayout != activeLayout) {
        currentLayout = activeLayout;
        osdStartFullRedraw();
    }
#endif

#define DRAW_FREQ_DENOM     4
#define STATS_FREQ_DENOM    50
    counter++;

    if ((counter % STATS_FREQ_DENOM) == 0) {
        osdUpdateStats();
    }

    if ((counter & DRAW_FREQ_DENOM) == 0) {
        // redraw values in buffer
        osdRefresh(currentTimeUs);
    } else {
        // rest of time redraw screen
        displayDrawScreen(osdDisplayPort);
    }

#ifdef USE_CMS
    // do not allow ARM if we are in menu
    if (displayIsGrabbed(osdDisplayPort)) {
        ENABLE_ARMING_FLAG(ARMING_DISABLED_OSD_MENU);
    } else {
        DISABLE_ARMING_FLAG(ARMING_DISABLED_OSD_MENU);
    }
#endif
}

void osdStartFullRedraw(void)
{
    fullRedraw = true;
}

void osdOverrideLayout(int layout)
{
    layoutOverride = constrain(layout, -1, ARRAYLEN(osdConfig()->item_pos) - 1);
}

bool osdItemIsFixed(osd_items_e item)
{
    return item == OSD_CROSSHAIRS ||
        item == OSD_ARTIFICIAL_HORIZON ||
        item == OSD_HORIZON_SIDEBARS;
}

#endif // OSD
