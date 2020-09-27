/****************************************************************************
 *   Copyright  2020  Jakub Vesely
 *   Email: jakub_vesely@seznam.cz
 ****************************************************************************/

/*
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
#include "config.h"
#include <TTGO.h>
#include <SPIFFS.h>

#include "rtcctl.h"
#include "powermgm.h"
#include "callback.h"

#include "hardware/powermgm.h"
#include "hardware/json_psram_allocator.h"

#include <time.h>

#define CONFIG_FILE_PATH "/rtcctr.json"

#define VERSION_KEY "version"
#define ENABLED_KEY "enabled"
#define HOUR_KEY "hour"
#define MINUTE_KEY "minute"
#define WEEK_DAYS_KEY "week_days" 

static rtcctl_alarm_t alarm_data = {
    .enabled = false,
    .hour = 0,
    .minute = 0,
    .week_days = {false, false, false, false, false, false, false,}
}; 
static time_t alarm_time = 0;

volatile bool DRAM_ATTR rtc_irq_flag = false;
portMUX_TYPE DRAM_ATTR RTC_IRQ_Mux = portMUX_INITIALIZER_UNLOCKED;
static void IRAM_ATTR rtcctl_irq( void );

bool rtcctl_powermgm_event_cb( EventBits_t event, void *arg );
bool rtcctl_powermgm_loop_cb( EventBits_t event, void *arg );
static void load_data();
callback_t *rtcctl_callback = NULL;

void rtcctl_setup( void ) {

    pinMode( RTC_INT, INPUT_PULLUP);
    attachInterrupt( RTC_INT, &rtcctl_irq, FALLING );

    powermgm_register_cb( POWERMGM_SILENCE_WAKEUP | POWERMGM_STANDBY | POWERMGM_WAKEUP, rtcctl_powermgm_event_cb, "rtcctl" );
    powermgm_register_loop_cb( POWERMGM_SILENCE_WAKEUP | POWERMGM_WAKEUP, rtcctl_powermgm_loop_cb, "rtcctl loop" );

    load_data();
}

static bool send_event_cb( EventBits_t event ) {
    return( callback_send( rtcctl_callback, event, (void*)NULL ) );
}

static bool is_any_day_enabled(){
    for (int index = 0; index < DAYS_IN_WEEK; ++index){
        if (alarm_data.week_days[index])
            return true; 
    }
    return false;
}

static time_t find_next_alarm_day(int day_of_week, time_t now){
    //it is expected that test if any day is enabled has been performed
    
    time_t ret_val = now;
    int wday_index = day_of_week;
    do {
        ret_val += 60 * 60 * 24; //number of seconds in day
        if (++wday_index == DAYS_IN_WEEK){
            wday_index = 0;
        } 
        if (alarm_data.week_days[wday_index]){
            return ret_val;
        }        
    } while (wday_index != day_of_week);
    
    return ret_val; //the same day of next week
}

static void set_next_alarm(TTGOClass *ttgo){
    if (!is_any_day_enabled()){
        ttgo->rtc->setAlarm( PCF8563_NO_ALARM, PCF8563_NO_ALARM, PCF8563_NO_ALARM, PCF8563_NO_ALARM );    
        send_event_cb( RTCCTL_ALARM_TERM_SET );
        return;
    } 

    //trc ans system must be synchronized, it is important when alarm has been raised and we want to set next concurency 
    //if the synchronisation is not done the time can be set to now again
    ttgo->rtc->syncToSystem(); 
    
    time_t now;
    time(&now);
    alarm_time = now;
    struct tm  alarm_tm;
    localtime_r(&alarm_time, &alarm_tm);
    alarm_tm.tm_hour = alarm_data.hour;
    alarm_tm.tm_min = alarm_data.minute;
    alarm_time = mktime(&alarm_tm);

    if (!alarm_data.week_days[alarm_tm.tm_wday] || alarm_time <= now){
       alarm_time = find_next_alarm_day( alarm_tm.tm_wday, alarm_time );
       localtime_r(&alarm_time, &alarm_tm);
    }
    //it is better define alarm by day in month rather than weekday. This way will be work-around an error in pcf8563 source and will avoid eaising alarm when there is only one alarm in the week (today) and alarm time is set to now
    ttgo->rtc->setAlarm( alarm_tm.tm_hour, alarm_tm.tm_min, alarm_tm.tm_mday, PCF8563_NO_ALARM );
    send_event_cb( RTCCTL_ALARM_TERM_SET );
}

void rtcctl_set_next_alarm(){
    TTGOClass *ttgo = TTGOClass::getWatch();
    if (alarm_data.enabled){
        ttgo->rtc->disableAlarm();
    }

    set_next_alarm(ttgo);
    
    if (alarm_data.enabled){
        ttgo->rtc->enableAlarm();
    }
}

bool rtcctl_powermgm_event_cb( EventBits_t event, void *arg ) {
    switch( event ) {
        case POWERMGM_STANDBY:          log_i("go standby");
                                        gpio_wakeup_enable( (gpio_num_t)RTC_INT, GPIO_INTR_LOW_LEVEL );
                                        esp_sleep_enable_gpio_wakeup ();
                                        break;
        case POWERMGM_WAKEUP:           log_i("go wakeup");
                                        break;
        case POWERMGM_SILENCE_WAKEUP:   log_i("go silence wakeup");
                                        break;
    }
    return( true );
}

bool rtcctl_powermgm_loop_cb( EventBits_t event, void *arg ) {
    rtcctl_loop();
    return( true );
}

static void IRAM_ATTR rtcctl_irq( void ) {
    portENTER_CRITICAL_ISR(&RTC_IRQ_Mux);
    rtc_irq_flag = true;
    portEXIT_CRITICAL_ISR(&RTC_IRQ_Mux);
    powermgm_set_event( POWERMGM_RTC_ALARM );
}

void rtcctl_loop( void ) {
    // fire callback
    if ( !powermgm_get_event( POWERMGM_STANDBY ) ) {
        portENTER_CRITICAL( &RTC_IRQ_Mux );
        bool temp_rtc_irq_flag = rtc_irq_flag;
        rtc_irq_flag = false;
        portEXIT_CRITICAL( &RTC_IRQ_Mux );
        if ( temp_rtc_irq_flag ) {
            send_event_cb( RTCCTL_ALARM_OCCURRED );
        }
    }
}

bool rtcctl_register_cb( EventBits_t event, CALLBACK_FUNC callback_func, const char *id ) {
    if ( rtcctl_callback == NULL ) {
        rtcctl_callback = callback_init( "rtctl" );
        if ( rtcctl_callback == NULL ) {
            log_e("rtcctl callback alloc failed");
            while(true);
        }
    }    
    return( callback_register( rtcctl_callback, event, callback_func, id ) );
}

static void load_data(){
    if (! SPIFFS.exists( CONFIG_FILE_PATH ) ) {
        return; //wil be used default values set during theier creation
    }

    fs::File file = SPIFFS.open( CONFIG_FILE_PATH, FILE_READ );
    if (!file) {
        log_e("Can't open file: %s!", CONFIG_FILE_PATH );
        return;
    }

    int filesize = file.size();
    SpiRamJsonDocument doc( filesize * 2 );

    DeserializationError error = deserializeJson( doc, file );
    if ( error ) {
        log_e("update check deserializeJson() failed: %s", error.c_str() );
        return;
    }

    rtcctl_alarm_t stored_data;
    stored_data.enabled = doc[ENABLED_KEY].as<bool>();
    stored_data.hour = doc[HOUR_KEY].as<uint8_t>();
    stored_data.minute =  doc[MINUTE_KEY].as<uint8_t>();
    uint8_t stored_week_days = doc[WEEK_DAYS_KEY].as<uint8_t>();
    for (int index = 0; index < DAYS_IN_WEEK; ++index){
        stored_data.week_days[index] = ((stored_week_days >> index) & 1) != 0;
    }
    rtcctl_set_alarm(&stored_data);

    doc.clear();
    file.close();
}

static void store_data(){
    if ( SPIFFS.exists( CONFIG_FILE_PATH ) ) {
        SPIFFS.remove( CONFIG_FILE_PATH );
        log_i("remove old binary rtcctl config");
    }

    fs::File file = SPIFFS.open( CONFIG_FILE_PATH, FILE_WRITE );
    if (!file) {
        log_e("Can't open file: %s!", CONFIG_FILE_PATH );
        return;
    }

    SpiRamJsonDocument doc( 1000 );

    doc[VERSION_KEY] = 1;
    doc[ENABLED_KEY] = alarm_data.enabled;
    doc[HOUR_KEY] = alarm_data.hour;
    doc[MINUTE_KEY] = alarm_data.minute;

    uint8_t week_days_to_store = 0;
    for (int index = 0; index < DAYS_IN_WEEK; ++index){
        week_days_to_store |= alarm_data.week_days[index] << index; 
    }
    doc[WEEK_DAYS_KEY] = week_days_to_store;
    
    if ( serializeJsonPretty( doc, file ) == 0) {
        log_e("Failed to write rtcctl config file");
    }

    doc.clear();
    file.close();
}

void rtcctl_set_alarm( rtcctl_alarm_t *data ){
    TTGOClass *ttgo = TTGOClass::getWatch();
    bool was_enabled = alarm_data.enabled;
    if (was_enabled){
        ttgo->rtc->disableAlarm();
    }
    alarm_data = *data;
    store_data();

    set_next_alarm(ttgo);

    if (was_enabled && !alarm_data.enabled){
        //already disabled
        send_event_cb( RTCCTL_ALARM_DISABLED );
    }
    else if (was_enabled && alarm_data.enabled){
        ttgo->rtc->enableAlarm();
        //nothing actually changed;
    }
    else if (!was_enabled && alarm_data.enabled){
        ttgo->rtc->enableAlarm();
        send_event_cb( RTCCTL_ALARM_ENABLED );   
    }    
}

rtcctl_alarm_t *rtcctl_get_alarm_data(){
    return &alarm_data;
}

int rtcctl_get_next_alarm_week_day(){
    if (!is_any_day_enabled()){
        return RTCCTL_ALARM_NOT_SET;
    }
    tm alarm_tm;
    localtime_r(&alarm_time, &alarm_tm);
    return alarm_tm.tm_wday;
}