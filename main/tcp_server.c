#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "driver/gpio.h"

#define PORT                        CONFIG_EXAMPLE_PORT
#define KEEPALIVE_IDLE              CONFIG_EXAMPLE_KEEPALIVE_IDLE
#define KEEPALIVE_INTERVAL          CONFIG_EXAMPLE_KEEPALIVE_INTERVAL
#define KEEPALIVE_COUNT             CONFIG_EXAMPLE_KEEPALIVE_COUNT

//tcp服务器开启监听的端口号
#define PORT_0      3333  //(PORT+0)
#define PORT_1      3334  //(PORT+1)
#define PORT_2      3335  //(PORT+2)
#define PORT_3      3336  //(PORT+3)
#define PORT_4      3337  //(PORT+4)
#define PORT_5      3338  //(PORT+5)

//自定义的tcp任务参数结构体
typedef struct TCP_SERVER_TASK_PARAM_t{
    int addr_family;  //地址类型，可赋值AF_INET,AF_INET6
    int sock;         //该任务处理的socket，只允许用socketBSD提供的api来对其读写
    int port;         //端口号
    int task_ID;      //处理该参数结构体的task的ID号，目前考虑和端口号port绑定
    char TAG[32]; //日志tag
}tcp_server_task_param_t;
//tcp任务参数初始化
static void tcp_task_param_init(tcp_server_task_param_t* param, int set_port, char* set_tag)
{//由于我自己水平不够，目前只提供ipv4的初始化，只能设置端口号和日志号
    param->addr_family = AF_INET;
    param->sock = 0;
    param->port = set_port;
    param->task_ID = set_port-PORT_0;
    strcpy(param->TAG,set_tag);
}

tcp_server_task_param_t tcp_task_param[6];//分别对应到六个任务上去

#define TASK_ID_MIN 0
#define TASK_ID_MAX 3 //目前app_main里面只开了四个tcp_task 0~3


static const char *BROADCAST_TAG = "broadcast";
static void do_broadcast(const int cur_sock,int cur_task_ID)
{
    //根据例程里的do_retransmit函数改过来的广播函数
    //cur_task_ID从调用该函数的任务里传过来，该参数用于排除自己端口以进行广播

    int len;
    char rx_buffer[128];

    do {
        len = recv(cur_sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
        
        if (len < 0) {
            ESP_LOGE(BROADCAST_TAG, "Error occurred during receiving: errno %d", errno);
        } else if (len == 0) {
            ESP_LOGW(BROADCAST_TAG, "Connection closed");
        } else {
            rx_buffer[len] = 0; // Null-terminate whatever is received and treat it like a string
            ESP_LOGI(BROADCAST_TAG, "Received %d bytes: %s", len, rx_buffer);

            int to_write = len;
            while (to_write > 0) {
                int written = 0;
                for(int tmp_ID = TASK_ID_MIN ; tmp_ID <= TASK_ID_MAX ; tmp_ID++){
                //对于每个id做广播（排除自己的id）
                    if(tmp_ID == cur_task_ID || tcp_task_param[tmp_ID].sock<=0) continue; // 跳过自己的id和跳过无效的socket
                    written=send(tcp_task_param[tmp_ID].sock, rx_buffer + (len - to_write), to_write, 0);
                }
                if (written < 0) {
                    ESP_LOGE(BROADCAST_TAG, "Error occurred during sending: errno %d", errno);
                }
                to_write -= written;
            }
        }
    } while (len > 0);
}

static int create_socket(struct sockaddr_in *dest_addr_ptr, uint16_t port,int addr_family)
{//返回一个按照参数配置的socket
    int ip_protocol = 0;
    if (addr_family == AF_INET) {//这是判断走ipv4的
        struct sockaddr_in *dest_addr_ip4 = dest_addr_ptr;
        dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4->sin_family = AF_INET;
        dest_addr_ip4->sin_port = htons(port);
        ip_protocol = IPPROTO_IP;
    }
    return socket(addr_family, SOCK_STREAM, ip_protocol);
}

static void tcp_server_task(void *voidp_task_param)
{
    tcp_server_task_param_t *task_param=(tcp_server_task_param_t*)voidp_task_param;  //任务参数的类型转换
    char addr_str[128];
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    struct sockaddr_storage dest_addr;

    //建立监听端口的sock
    int listen_sock = create_socket((struct sockaddr_in *)&dest_addr,task_param->port,task_param->addr_family);
    if (listen_sock < 0) {
        ESP_LOGE(task_param->TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ESP_LOGI(task_param->TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(task_param->TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(task_param->TAG, "IPPROTO: %d", task_param->addr_family);
        close(listen_sock);vTaskDelete(NULL);return ;//做CLEAN_UP;
    }
    ESP_LOGI(task_param->TAG, "Socket bound, port %d", task_param->port);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(task_param->TAG, "Error occurred during listen: errno %d", errno);   
        close(listen_sock);vTaskDelete(NULL);return ;//做CLEAN_UP;
    }

    while (1) {
        ESP_LOGI(task_param->TAG, "Socket listening");
        struct sockaddr_storage source_addr;
        socklen_t addr_len = sizeof(source_addr);
        
        task_param->sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);  //获取客户端的ip地址
        
        if (task_param->sock < 0) {
            ESP_LOGE(task_param->TAG, "Unable to accept connection: errno %d", errno);
            break;
        }
        
        // Set tcp keepalive option
        setsockopt(task_param->sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(task_param->sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(task_param->sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(task_param->sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
            
        // Convert ip address to string
        if (source_addr.ss_family == PF_INET) 
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        ESP_LOGI(task_param->TAG, "Socket accepted ip address: %s", addr_str);
        
        do_broadcast(task_param->sock,task_param->task_ID);  //做tcp广播
        
        shutdown(task_param->sock,0);
        close(task_param->sock);
    }
    close(listen_sock);
    vTaskDelete(NULL);
}

#define WIFI_SSID "esp32_ap_test"
#define WIFI_PASSWORD "23456789"
#define WIFI_MAX_STA_CONN 4
static const char *WIFI_EVENT_TAG = "wifi event";
uint8_t ApMac[6];//wifi网卡的MAC
static void wifi_event_handler(void* arg, esp_event_base_t event_base,int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(WIFI_EVENT_TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(WIFI_EVENT_TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}
static void initialize_wifi(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, 
                                               ESP_EVENT_ANY_ID,
                                               wifi_event_handler,
                                               NULL));  //注册wifi事件
    esp_wifi_get_mac( ESP_IF_WIFI_AP , ApMac );

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = (WIFI_SSID),
            .ssid_len = strlen(WIFI_SSID),
            .password = WIFI_PASSWORD,
            .max_connection = WIFI_MAX_STA_CONN,  //同时接入ap的sta的数量上限
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .channel = 1 // default: channel 1
        },
    };
    if (strlen(WIFI_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());   
    
    ESP_LOGI("softAP", "SSID:%s    PASSWORD:%s    MAC:%02X-%02X-%02X-%02X-%02X-%02X", \
            WIFI_SSID,\
            WIFI_PASSWORD,\
            ApMac[0], ApMac[1], ApMac[2], ApMac[3], ApMac[4], ApMac[5] );

    
    //set IP address
    tcpip_adapter_ip_info_t ip_info = {        
        .ip.addr = ipaddr_addr("192.168.100.1"),        
        .netmask.addr = ipaddr_addr("255.255.255.0"),        
        .gw.addr      = ipaddr_addr("192.168.100.1"),    
    };    
    ESP_ERROR_CHECK(tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP));    //连进来的sta开静态ip
    ESP_ERROR_CHECK(tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &ip_info));    
    //ESP_ERROR_CHECK(tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP));
    printf("self:"IPSTR"\n",IP2STR(&ip_info.ip));
    printf("self:"IPSTR"\n",IP2STR(&ip_info.netmask));
    printf("self:"IPSTR"\n",IP2STR(&ip_info.gw));
}


#define  LED_GPIO_NUM  GPIO_NUM_2
inline void led_init( void )
{gpio_set_direction( LED_GPIO_NUM , GPIO_MODE_OUTPUT );}
void led_task( void *pvParameters )
{//点灯
    uint8_t level = 0;
    led_init();
    while(1)
    {
        gpio_set_level( LED_GPIO_NUM , level);
        level = !level;
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void app_main(void)
{
    //init NVS flash
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    //init wifi netif
    ESP_ERROR_CHECK(esp_netif_init());  //初始化LwIP
    ESP_ERROR_CHECK(esp_event_loop_create_default());  //初始化loop event  
    initialize_wifi();

    //初始化tcp监听任务的参数结构体
    tcp_task_param_init(&tcp_task_param[0], PORT_0 ,"tcp server 0");
    tcp_task_param_init(&tcp_task_param[1], PORT_1 ,"tcp server 1");
    tcp_task_param_init(&tcp_task_param[2], PORT_2 ,"tcp server 2");
    tcp_task_param_init(&tcp_task_param[3], PORT_3 ,"tcp server 3");
    
    //FreeRTOS task create
    xTaskCreate( &led_task, "led task", 512, NULL, 3, NULL ); //建立闪灯任务
    xTaskCreate(tcp_server_task, "tcp_server_0", 4096, (void *)&tcp_task_param[0], 5, NULL);
    xTaskCreate(tcp_server_task, "tcp_server_1", 4096, (void *)&tcp_task_param[1], 5, NULL);
    xTaskCreate(tcp_server_task, "tcp_server_2", 4096, (void *)&tcp_task_param[2], 5, NULL);
    xTaskCreate(tcp_server_task, "tcp_server_3", 4096, (void *)&tcp_task_param[3], 5, NULL);
}
