/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain
 * this notice you can do whatever you want with this stuff. If we meet some day,
 * and you think this stuff is worth it, you can buy me a beer in return.
 * ----------------------------------------------------------------------------
 */

/*
This is example code for the esphttpd library. It's a small-ish demo showing off
the server, including WiFi connection management capabilities, some IO etc.
*/
#include "sdkconfig.h"

#include <libesphttpd/esp.h>
#include "libesphttpd/httpd.h"
#include "io.h"

#ifdef CONFIG_ESPHTTPD_USE_ESPFS
#include "espfs.h"
#include "espfs_image.h"
#include "libesphttpd/httpd-espfs.h"
#endif // CONFIG_ESPHTTPD_USE_ESPFS

#include "cgi.h"
#include "libesphttpd/cgiwifi.h"
#include "libesphttpd/cgiflash.h"
#include "libesphttpd/auth.h"
#include "libesphttpd/captdns.h"
#include "libesphttpd/cgiwebsocket.h"
#include "libesphttpd/httpd-freertos.h"
#include "libesphttpd/route.h"
#include "cgi-test.h"

#include "esp_wifi.h"

#if defined(CONFIG_WMNGR_ENABLED)
#include "wifi_manager.h"
#endif /* defined(CONFIG_WMNGR_ENABLED) */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#ifdef ESP32
#include "freertos/event_groups.h"
#include "esp_log.h"

#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_event_loop.h"
#include "esp_event.h"
#include "tcpip_adapter.h"


char my_hostname[16] = "esphttpd";

/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_WIFI_SSID      CONFIG_EXAMPLE_WIFI_SSID
#define EXAMPLE_WIFI_PASS      CONFIG_EXAMPLE_WIFI_PASSWORD
#endif

#define TAG "user_main"

#define LISTEN_PORT     80u
#define MAX_CONNECTIONS 32u

static char connectionMemory[sizeof(RtosConnType) * MAX_CONNECTIONS];
static HttpdFreertosInstance httpdFreertosInstance;

//Function that tells the authentication system what users/passwords live on the system.
//This is disabled in the default build; if you want to try it, enable the authBasic line in
//the builtInUrls below.
int myPassFn(HttpdConnData *connData, int no, char *user, int userLen, char *pass, int passLen) {
	if (no==0) {
		strcpy(user, "admin");
		strcpy(pass, "s3cr3t");
		return 1;
//Add more users this way. Check against incrementing no for each user added.
//	} else if (no==1) {
//		strcpy(user, "user1");
//		strcpy(pass, "something");
//		return 1;
	}
	return 0;
}


//Broadcast the uptime in seconds every second over connected websockets
static void websocketBcast(void *arg) {
	static int ctr=0;
	char buff[128];
	while(1) {
		ctr++;
		sprintf(buff, "Up for %d minutes %d seconds!\n", ctr/60, ctr%60);
		cgiWebsockBroadcast(&httpdFreertosInstance.httpdInstance,
		                    "/websocket/ws.cgi", buff, strlen(buff),
		                    WEBSOCK_FLAG_NONE);

		vTaskDelay(1000/portTICK_RATE_MS);
	}
}

//On reception of a message, send "You sent: " plus whatever the other side sent
static void myWebsocketRecv(Websock *ws, char *data, int len, int flags) {
	int i;
	char buff[128];
	sprintf(buff, "You sent: ");
	for (i=0; i<len; i++) buff[i+10]=data[i];
	buff[i+10]=0;
	cgiWebsocketSend(&httpdFreertosInstance.httpdInstance,
	                 ws, buff, strlen(buff), WEBSOCK_FLAG_NONE);
}

//Websocket connected. Install reception handler and send welcome message.
static void myWebsocketConnect(Websock *ws) {
	ws->recvCb=myWebsocketRecv;
	cgiWebsocketSend(&httpdFreertosInstance.httpdInstance,
	                 ws, "Hi, Websocket!", 14, WEBSOCK_FLAG_NONE);
}

//On reception of a message, echo it back verbatim
void myEchoWebsocketRecv(Websock *ws, char *data, int len, int flags) {
	printf("EchoWs: echo, len=%d\n", len);
	cgiWebsocketSend(&httpdFreertosInstance.httpdInstance,
	                 ws, data, len, flags);
}

//Echo websocket connected. Install reception handler.
void myEchoWebsocketConnect(Websock *ws) {
	printf("EchoWs: connect\n");
	ws->recvCb=myEchoWebsocketRecv;
}

#define OTA_FLASH_SIZE_K 1024
#define OTA_TAGNAME "generic"

CgiUploadFlashDef uploadParams={
	.type=CGIFLASH_TYPE_FW,
	.fw1Pos=0x1000,
	.fw2Pos=((OTA_FLASH_SIZE_K*1024)/2)+0x1000,
	.fwSize=((OTA_FLASH_SIZE_K*1024)/2)-0x1000,
	.tagName=OTA_TAGNAME
};


/*
This is the main url->function dispatching data struct.
In short, it's a struct with various URLs plus their handlers. The handlers can
be 'standard' CGI functions you wrote, or 'special' CGIs requiring an argument.
They can also be auth-functions. An asterisk will match any url starting with
everything before the asterisks; "*" matches everything. The list will be
handled top-down, so make sure to put more specific rules above the more
general ones. Authorization things (like authBasic) act as a 'barrier' and
should be placed above the URLs they protect.
*/
HttpdBuiltInUrl builtInUrls[]={
	ROUTE_CGI_ARG("*", cgiRedirectApClientToHostname, "esp8266.nonet"),
	ROUTE_REDIRECT("/", "/index.tpl"),

	ROUTE_TPL("/led.tpl", tplLed),
	ROUTE_TPL("/index.tpl", tplCounter),
	ROUTE_CGI("/led.cgi", cgiLed),

	ROUTE_REDIRECT("/flash", "/flash/index.html"),
	ROUTE_REDIRECT("/flash/", "/flash/index.html"),
	ROUTE_CGI("/flash/flashinfo.json", cgiGetFlashInfo),
	ROUTE_CGI("/flash/setboot", cgiSetBoot),
	ROUTE_CGI_ARG("/flash/upload", cgiUploadFirmware, &uploadParams),
	ROUTE_CGI_ARG("/flash/erase", cgiEraseFlash, &uploadParams),
	ROUTE_CGI("/flash/reboot", cgiRebootFirmware),

#if defined(CONFIG_WMNGR_ENABLED)
	//Routines to make the /wifi URL and everything beneath it work.
//Enable the line below to protect the WiFi configuration with an username/password combo.
//	{"/wifi/*", authBasic, myPassFn},

	ROUTE_REDIRECT("/wifi", "/wifi/wifi.tpl"),
	ROUTE_REDIRECT("/wifi/", "/wifi/wifi.tpl"),
	ROUTE_CGI("/wifi/wifiscan.cgi", cgiWiFiScan),
	ROUTE_TPL("/wifi/wifi.tpl", tplWlan),
	ROUTE_CGI("/wifi/connect.cgi", cgiWiFiConnect),
	ROUTE_CGI("/wifi/connstatus.cgi", cgiWiFiConnStatus),
	ROUTE_CGI("/wifi/setmode.cgi", cgiWiFiSetMode),
	ROUTE_CGI("/wifi/startwps.cgi", cgiWiFiStartWps),
#endif /* defined(CONFIG_WMNGR_ENABLED) */

	ROUTE_REDIRECT("/websocket", "/websocket/index.html"),
	ROUTE_WS("/websocket/ws.cgi", myWebsocketConnect),
	ROUTE_WS("/websocket/echo.cgi", myEchoWebsocketConnect),

	ROUTE_REDIRECT("/httptest", "/httptest/index.html"),
	ROUTE_REDIRECT("/httptest/", "/httptest/index.html"),
	ROUTE_CGI("/httptest/test.cgi", cgiTestbed),

	ROUTE_FILESYSTEM(),

	ROUTE_END()
};


#ifdef ESP32


static void dump_sta_info(void)
{
	tcpip_adapter_ip_info_t sta_ip_info;
	wifi_config_t sta_conf;

	printf("~~~~~STA~~~~~" "\n");
	if (esp_wifi_get_config(TCPIP_ADAPTER_IF_STA, &sta_conf) == ESP_OK) {
		printf("ssid: %s" "\n", sta_conf.sta.ssid);
	} else {
		printf("Error fetching WiFi config.\n");
	}

	if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &sta_ip_info) == ESP_OK) {
		printf("IP:" IPSTR "\n", IP2STR(&sta_ip_info.ip));
		printf("MASK:" IPSTR "\n", IP2STR(&sta_ip_info.netmask));
		printf("GW:" IPSTR "\n", IP2STR(&sta_ip_info.gw));
	} else {
		printf("Error fetching IP info.\n");
	}
	printf("~~~~~~~~~~~~~" "\n");
}

static void dump_ap_info(void)
{
	tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_AP, my_hostname);
	tcpip_adapter_ip_info_t ap_ip_info;
	wifi_config_t ap_conf;

	printf("~~~~~AP~~~~~" "\n");
	if (esp_wifi_get_config(TCPIP_ADAPTER_IF_AP, &ap_conf) == ESP_OK) {
		printf("ssid: %s" "\n", ap_conf.ap.ssid);
		if (ap_conf.ap.authmode != WIFI_AUTH_OPEN){
			printf("pass: %s" "\n", ap_conf.ap.password);
		}
	} else {
		printf("Error fetching WiFi config.\n");
	}

	if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ap_ip_info) == ESP_OK) {
		printf("IP:" IPSTR "\n", IP2STR(&ap_ip_info.ip));
		printf("MASK:" IPSTR "\n", IP2STR(&ap_ip_info.netmask));
		printf("GW:" IPSTR "\n", IP2STR(&ap_ip_info.gw));
	} else {
		printf("Error fetching IP info.\n");
	}
	printf("~~~~~~~~~~~~" "\n");
}

static esp_err_t app_event_handler(void *ctx, system_event_t *event)
{
	switch(event->event_id) {
	case SYSTEM_EVENT_ETH_START:
		tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_ETH, my_hostname);
		break;
	case SYSTEM_EVENT_STA_START:
		tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, my_hostname);
#if !defined(CONFIG_WMNGR_ENABLED)
		esp_wifi_connect();
#endif /* !defined(CONFIG_WMNGR_ENABLED) */
		break;
	case SYSTEM_EVENT_STA_GOT_IP:
		dump_sta_info();
		set_status_ind_wifi(WIFI_STATE_CONN);
		break;
	case SYSTEM_EVENT_STA_CONNECTED:
		break;
	case SYSTEM_EVENT_STA_DISCONNECTED:
#if !defined(CONFIG_WMNGR_ENABLED)
		/* This is a workaround as ESP32 WiFi libs don't currently *\
		\* auto-reassociate.                                       */
		esp_wifi_connect();
#endif /* !defined(CONFIG_WMNGR_ENABLED) */
		break;
	case SYSTEM_EVENT_AP_START:
		dump_ap_info();
		set_status_ind_wifi(WIFI_STATE_AP);
		break;
	case SYSTEM_EVENT_AP_STACONNECTED:
		ESP_LOGI(TAG, "station:" MACSTR" join,AID=%d",
				MAC2STR(event->event_info.sta_connected.mac),
				event->event_info.sta_connected.aid);

		set_status_ind_wifi(WIFI_STATE_CONN);
		break;
	case SYSTEM_EVENT_AP_STADISCONNECTED:
		ESP_LOGI(TAG, "station:" MACSTR"leave,AID=%d",
				MAC2STR(event->event_info.sta_disconnected.mac),
				event->event_info.sta_disconnected.aid);

/* FIXME: Handle WiFi Manager transition from AP+STA to AP-only mode. */
#if !defined(CONFIG_WMNGR_ENABLED)
		{
			wifi_sta_list_t sta_list;
			ESP_ERROR_CHECK( esp_wifi_ap_get_sta_list(&sta_list));
			if (sta_list.num == 0) {
				/* No clients left, change blink indication to AP */
				set_status_ind_wifi(WIFI_STATE_AP);
			}
		}
#endif /* !defined(CONFIG_WMNGR_ENABLED) */
		break;
	default:
		break;
	}

	return ESP_OK;
}

#if !defined(CONFIG_WMNGR_ENABLED)
//Simple task to connect to an access point
void init_wifi(bool modeAP) {
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
	ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );

	//Go to station mode
	esp_wifi_disconnect();

	if(modeAP) {
		ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_AP) );

		wifi_config_t ap_config;
		strcpy((char*)(&ap_config.ap.ssid), "ESP");
		ap_config.ap.ssid_len = 3;
		ap_config.ap.channel = 1;
		ap_config.ap.authmode = WIFI_AUTH_OPEN;
		ap_config.ap.ssid_hidden = 0;
		ap_config.ap.max_connection = 3;
		ap_config.ap.beacon_interval = 100;

		esp_wifi_set_config(WIFI_IF_AP, &ap_config);
	}
	else {
		esp_wifi_set_mode(WIFI_MODE_STA);

		//Connect to the defined access point.
		wifi_config_t config;
		memset(&config, 0, sizeof(config));
		sprintf((char*)config.sta.ssid, EXAMPLE_WIFI_SSID);
		sprintf((char*)config.sta.password, EXAMPLE_WIFI_PASS);
		esp_wifi_set_config(WIFI_IF_STA, &config);
		esp_wifi_connect();
	}

	ESP_ERROR_CHECK( esp_wifi_start() );
}
#endif /* defined(CONFIG_WMNGR_ENABLED) */
#endif /* ESP32 */

//Main routine. Initialize stdout, the I/O, filesystem and the webserver and we're done.
#if ESP32
void app_main(void) {
#else
void user_init(void) {
#endif
#if defined(ESP32)
	esp_err_t result;
#endif

#ifndef ESP32
	uart_div_modify(0, UART_CLK_FREQ / 115200);
#endif

	ioInit();
// FIXME: Re-enable this when capdns is fixed for esp32
//	captdnsInit();

#ifdef CONFIG_ESPHTTPD_USE_ESPFS
	espFsInit((void*)(image_espfs_start));
#endif // CONFIG_ESPHTTPD_USE_ESPFS

#if defined(ESP32)
	result = nvs_flash_init();
	if(result == ESP_ERR_NVS_NO_FREE_PAGES
	|| result == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_LOGI(TAG, "Erasing NVS");
		nvs_flash_erase();
		result = nvs_flash_init();
	}
	ESP_ERROR_CHECK(result);

	ESP_ERROR_CHECK( esp_event_loop_init(app_event_handler, NULL) );
#endif /* defined(ESP32) */

	tcpip_adapter_init();
	httpdFreertosInit(&httpdFreertosInstance,
	                  builtInUrls,
	                  LISTEN_PORT,
	                  connectionMemory,
	                  MAX_CONNECTIONS,
	                  HTTPD_FLAG_NONE);
	httpdFreertosStart(&httpdFreertosInstance);

#if defined(CONFIG_WMNGR_ENABLED)
	ESP_ERROR_CHECK(esp_wmngr_init());
#else
	init_wifi(true); // Supply false for STA mode
#endif
	xTaskCreate(websocketBcast, "wsbcast", 3000, NULL, 3, NULL);

	printf("\nReady\n");
}
