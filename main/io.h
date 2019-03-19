#ifndef IO_H
#define IO_H

#include "driver/gpio.h"

#define LED_WIFI (GPIO_NUM_5)  // Green LED
#define LED_BLE (GPIO_NUM_15) // Blue LED
#define LED_CGI (GPIO_NUM_32)  // Red LED

enum BLE_state_e {
	BLE_STATE_OFF = 0,  // BLE turned-off
	BLE_STATE_ADV = 1,  // BLE is advertising but not connected
	BLE_STATE_CONN = 2, // BLE connected to a central
	BLE_STATE_FAIL,
	BLE_STATE_NUM_STATES
};
void set_status_ind_ble(enum BLE_state_e new_state);

enum WIFI_state_e {
	WIFI_STATE_OFF = 0,  // either off or set to STA but not connected
	WIFI_STATE_AP = 1,   // AP enabled but no clients connected
	WIFI_STATE_CONN = 2, // WiFi connected to 1+ peer either as AP or STA
	WIFI_STATE_FAIL,
	WIFI_STATE_NUM_STATES
};
void set_status_ind_wifi(enum WIFI_state_e new_state);

#define BTN_GPIO  (GPIO_NUM_34) // Push-button


void ioLed(int ledgpio, int en);
void ioInit(void);

#endif
