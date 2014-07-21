/**
  ******************************************************************************
  * @file    MICOEntrance.c 
  * @author  William Xu
  * @version V1.0.0
  * @date    05-May-2014
  * @brief   MICO system main entrance.
  ******************************************************************************
  * @attention
  *
  * THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
  * WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
  * TIME. AS A RESULT, MXCHIP Inc. SHALL NOT BE HELD LIABLE FOR ANY
  * DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
  * FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE
  * CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
  *
  * <h2><center>&copy; COPYRIGHT 2014 MXCHIP Inc.</center></h2>
  ******************************************************************************
  */ 

#include "Platform.h"
#include "MICO.h"
#include "MICODefine.h"
#include "MICOAppDefine.h"

#include "MICONotificationCenter.h"
#include "MICOSystemMonitor.h"
#include "EasyLink/EasyLink.h"
#include "StringUtils.h"

#ifdef CONFIG_MODE_EASYLINK
#include "EasyLink/EasyLink.h"
#endif

#ifdef CONFIG_MODE_WAC
#include "WAC/MFi_WAC.h"
#endif

static mico_Context_t *context;
static mico_timer_t _watchdog_reload_timer;

static mico_system_monitor_t mico_monitor;

const char *eaProtocols[1] = {EA_PROTOCOL};

#define mico_log(M, ...) custom_log("MICO", M, ##__VA_ARGS__)
#define mico_log_trace() custom_log_trace("MICO")

__weak void sendNotifySYSWillPowerOff(void){

}

/* ========================================
User provide callback functions 
======================================== */
void micoNotify_ReadAppInfoHandler(char *str, int len, mico_Context_t * const inContext)
{
  (void)inContext;
  snprintf( str, len, "%s", APP_INFO);
}   


void PlatformEasyLinkButtonClickedCallback(void)
{
  mico_log_trace();
  if(context->flashContentInRam.micoSystemConfig.configured == allConfigured){
    context->flashContentInRam.micoSystemConfig.configured = wLanUnConfigured;
    MICOUpdateConfiguration(context);
  }
  context->micoStatus.sys_state = eState_Software_Reset;
  require(context->micoStatus.sys_state_change_sem, exit);
  mico_rtos_set_semaphore(&context->micoStatus.sys_state_change_sem);
exit: 
  return;
}

void PlatformEasyLinkButtonLongPressedCallback(void)
{
  mico_log_trace();
  MICORestoreDefault(context);
  context->micoStatus.sys_state = eState_Software_Reset;
  require(context->micoStatus.sys_state_change_sem, exit);
  mico_rtos_set_semaphore(&context->micoStatus.sys_state_change_sem);
exit: 
  return;
}

 void PlatformStandbyButtonClickedCallback(void)
 {
    mico_log_trace();
    context->micoStatus.sys_state = eState_Standby;
    require(context->micoStatus.sys_state_change_sem, exit);
    mico_rtos_set_semaphore(&context->micoStatus.sys_state_change_sem);
exit: 
    return;
 }

void micoNotify_WifiStatusHandler(WiFiEvent event, mico_Context_t * const inContext)
{
  mico_log_trace();
  (void)inContext;
  switch (event) {
  case NOTIFY_STATION_UP:
    mico_log("Station up");
    break;
  case NOTIFY_STATION_DOWN:
    mico_log("Station down");
    break;
  default:
    break;
  }
  return;
}

void micoNotify_DHCPCompleteHandler(net_para_st *pnet, mico_Context_t * const inContext)
{
  mico_log_trace();
  require(inContext, exit);
  mico_rtos_lock_mutex(&inContext->flashContentInRam_mutex);
  strcpy((char *)inContext->micoStatus.localIp, pnet->ip);
  strcpy((char *)inContext->micoStatus.netMask, pnet->mask);
  strcpy((char *)inContext->micoStatus.gateWay, pnet->gate);
  strcpy((char *)inContext->micoStatus.dnsServer, pnet->dns);
  mico_rtos_unlock_mutex(&inContext->flashContentInRam_mutex);
exit:
  return;
}

void micoNotify_WiFIParaChangedHandler(apinfo_adv_t *ap_info, char *key, int key_len, mico_Context_t * const inContext)
{
  mico_log_trace();
  bool _needsUpdate = false;
  require(inContext, exit);
  mico_rtos_lock_mutex(&inContext->flashContentInRam_mutex);
  if(strncmp(inContext->flashContentInRam.micoSystemConfig.ssid, ap_info->ssid, maxSsidLen)!=0){
    strncpy(inContext->flashContentInRam.micoSystemConfig.ssid, ap_info->ssid, maxSsidLen);
    _needsUpdate = true;
  }

  if(memcmp(inContext->flashContentInRam.micoSystemConfig.bssid, ap_info->bssid, 6)!=0){
    memcpy(inContext->flashContentInRam.micoSystemConfig.bssid, ap_info->bssid, 6);
    _needsUpdate = true;
  }

  if(inContext->flashContentInRam.micoSystemConfig.channel != ap_info->channel){
    inContext->flashContentInRam.micoSystemConfig.channel = ap_info->channel;
    _needsUpdate = true;
  }
  
  if(inContext->flashContentInRam.micoSystemConfig.security != ap_info->security){
    inContext->flashContentInRam.micoSystemConfig.security = ap_info->security;
    _needsUpdate = true;
  }

  if(memcmp(inContext->flashContentInRam.micoSystemConfig.key, key, maxKeyLen)!=0){
    memcpy(inContext->flashContentInRam.micoSystemConfig.key, key, maxKeyLen);
    _needsUpdate = true;
  }

  if(inContext->flashContentInRam.micoSystemConfig.keyLength != key_len){
    inContext->flashContentInRam.micoSystemConfig.keyLength = key_len;
    _needsUpdate = true;
  }

  if(_needsUpdate== true)  
    MICOUpdateConfiguration(inContext);
  mico_rtos_unlock_mutex(&inContext->flashContentInRam_mutex);
  
exit:
  return;
}

void _ConnectToAP( mico_Context_t * const inContext)
{
  mico_log_trace();
  network_InitTypeDef_adv_st wNetConfig;
  mico_log("connect to %s.....", inContext->flashContentInRam.micoSystemConfig.ssid);
  memset(&wNetConfig, 0x0, sizeof(network_InitTypeDef_adv_st));
  
  mico_rtos_lock_mutex(&inContext->flashContentInRam_mutex);
  strncpy((char*)wNetConfig.ap_info.ssid, inContext->flashContentInRam.micoSystemConfig.ssid, maxSsidLen);
  memcpy(wNetConfig.ap_info.bssid, inContext->flashContentInRam.micoSystemConfig.bssid, 6);
  wNetConfig.ap_info.channel = inContext->flashContentInRam.micoSystemConfig.channel;
  wNetConfig.ap_info.security = inContext->flashContentInRam.micoSystemConfig.security;
  memcpy(wNetConfig.key, inContext->flashContentInRam.micoSystemConfig.key, inContext->flashContentInRam.micoSystemConfig.keyLength);
  wNetConfig.key_len = inContext->flashContentInRam.micoSystemConfig.keyLength;
  if(inContext->flashContentInRam.micoSystemConfig.dhcpEnable == true)
    wNetConfig.dhcpMode = DHCP_Client;
  else
    wNetConfig.dhcpMode = DHCP_Disable;
  strncpy((char*)wNetConfig.local_ip_addr, inContext->flashContentInRam.micoSystemConfig.localIp, maxIpLen);
  strncpy((char*)wNetConfig.net_mask, inContext->flashContentInRam.micoSystemConfig.netMask, maxIpLen);
  strncpy((char*)wNetConfig.gateway_ip_addr, inContext->flashContentInRam.micoSystemConfig.gateWay, maxIpLen);
  strncpy((char*)wNetConfig.dnsServer_ip_addr, inContext->flashContentInRam.micoSystemConfig.dnsServer, maxIpLen);

  mico_rtos_unlock_mutex(&inContext->flashContentInRam_mutex);

  wNetConfig.wifi_retry_interval = 100;
  StartAdvNetwork(&wNetConfig);
}

static void _watchdog_reload_timer_handler( void* arg )
{
  (void)(arg);
  MICOUpdateSystemMonitor(&mico_monitor, APPLICATION_WATCHDOG_TIMEOUT_SECONDS*1000-100);
}

int application_start(void)
{
  OSStatus err = kNoErr;
  net_para_st para;

  Platform_Init();
  /*Read current configurations*/
  context = ( mico_Context_t *)malloc(sizeof(mico_Context_t) );
  require_action( context, exit, err = kNoMemoryErr );
  memset(context, 0x0, sizeof(mico_Context_t));
  mico_rtos_init_mutex(&context->flashContentInRam_mutex);
  mico_rtos_init_semaphore(&context->micoStatus.sys_state_change_sem, 1); 

  MICOReadConfiguration( context );

  err = MICOInitNotificationCenter  ( context );

  err = MICOAddNotification( mico_notify_READ_APP_INFO, (void *)micoNotify_ReadAppInfoHandler );
  require_noerr( err, exit );  
  
  /*wlan driver and tcpip init*/
  mxchipInit();
  getNetPara(&para, Station);
  formatMACAddr(context->micoStatus.mac, (char *)&para.mac);
  
  mico_log_trace(); 
  mico_log("%s mxchipWNet library version: %s", APP_INFO, system_lib_version());

  /*Start system monotor thread*/
  err = MICOStartSystemMonitor(context);
  require_noerr_action( err, exit, mico_log("ERROR: Unable to start the system monitor.") );
  
  err = MICORegisterSystemMonitor(&mico_monitor, APPLICATION_WATCHDOG_TIMEOUT_SECONDS*1000);
  require_noerr( err, exit );
  mico_init_timer(&_watchdog_reload_timer,APPLICATION_WATCHDOG_TIMEOUT_SECONDS*1000-100, _watchdog_reload_timer_handler, NULL);
  mico_start_timer(&_watchdog_reload_timer);
  
  if(context->flashContentInRam.micoSystemConfig.configured != allConfigured){
    mico_log("Empty configuration. Starting configuration mode...");

#ifdef CONFIG_MODE_EASYLINK
  err = startEasyLink( context );
  require_noerr( err, exit );
#endif

#ifdef CONFIG_MODE_WAC
  WACPlatformParameters_t* WAC_Params = NULL;
  WAC_Params = calloc(1, sizeof(WACPlatformParameters_t));
  require(WAC_Params, exit);

  str2hex((unsigned char *)para.mac, WAC_Params->macAddress, 6);
  WAC_Params->isUnconfigured          = 1;
  WAC_Params->supportsAirPlay         = 0;
  WAC_Params->supportsAirPrint        = 0;
  WAC_Params->supports2_4GHzWiFi      = 1;
  WAC_Params->supports5GHzWiFi        = 0;
  WAC_Params->supportsWakeOnWireless  = 0;

  WAC_Params->firmwareRevision =  FIRMWARE_REVISION;
  WAC_Params->hardwareRevision =  HARDWARE_REVISION;
  WAC_Params->serialNumber =      SERIAL_NUMBER;
  WAC_Params->name =              context->flashContentInRam.micoSystemConfig.name;
  WAC_Params->model =             MODEL;
  WAC_Params->manufacturer =      MANUFACTURER;

  WAC_Params->numEAProtocols =    1;
  WAC_Params->eaBundleSeedID =    BUNDLE_SEED_ID;
  WAC_Params->eaProtocols =       (char **)eaProtocols;;

  err = startMFiWAC( context, WAC_Params, 1200);
  free(WAC_Params);
  require_noerr( err, exit );
#endif
  }
  else{
    mico_log("Available configuration. Starting Wi-Fi connection...");
    
    /* Regisist notifications */
    err = MICOAddNotification( mico_notify_WIFI_STATUS_CHANGED, (void *)micoNotify_WifiStatusHandler );
    require_noerr( err, exit ); 
    
    err = MICOAddNotification( mico_notify_WiFI_PARA_CHANGED, (void *)micoNotify_WiFIParaChangedHandler );
    require_noerr( err, exit ); 

    err = MICOAddNotification( mico_notify_DHCP_COMPLETED, (void *)micoNotify_DHCPCompleteHandler );
    require_noerr( err, exit );  
   
    if(context->flashContentInRam.micoSystemConfig.rfPowerSaveEnable == true){
      ps_enable();
    }

    if(context->flashContentInRam.micoSystemConfig.mcuPowerSaveEnable == true){
      mico_mcu_powersave_config(true);
    }

    /*Local configuration server*/
    if(context->flashContentInRam.micoSystemConfig.configServerEnable == true){
      err =  MICOStartConfigServer(context);
      require_noerr_action( err, exit, mico_log("ERROR: Unable to start the local server thread.") );
    }

    /*Start mico application*/
    err = MICOStartApplication( context );
    require_noerr( err, exit );
    
    _ConnectToAP( context );
  }


  /*System status changed*/
  while(mico_rtos_get_semaphore(&context->micoStatus.sys_state_change_sem, MICO_WAIT_FOREVER)==kNoErr){
    switch(context->micoStatus.sys_state){
      case eState_Normal:
        break;
      case eState_Software_Reset:
        sendNotifySYSWillPowerOff();
        mico_thread_msleep(500);
        PlatformSoftReboot();
        break;
      case eState_Wlan_Powerdown:
        sendNotifySYSWillPowerOff();
        mico_thread_msleep(500);
        wifi_power_down();
        break;
      case eState_Standby:
        mico_log("Enter standby mode");
        sendNotifySYSWillPowerOff();
        mico_thread_msleep(200);
        wifi_power_down();
        Platform_Enter_STANDBY();
        break;
      default:
        break;
    }
  }
    
  require_noerr_action( err, exit, mico_log("Closing main thread with err num: %d.", err) );

exit:
  mico_rtos_delete_thread(NULL);
  return kNoErr;
}


