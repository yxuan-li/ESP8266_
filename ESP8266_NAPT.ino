#define DBG_Printf_Enable true

#if (DBG_Printf_Enable == true)
  #define BLINKER_PRINT Serial
#endif
#define ArduinoOTA_Enable true

#define LED_PIN 2 //GPIO2
#define KEY_PIN 0 //GPIO0

//定义极性
#define LED_ON LOW
#define LED_OFF HIGH

#define WifiManager_ConnectTimeout 6*60//6mins
#define WifiManager_ConfigPortalTimeout 5*60//5mins
#define NAPT 1000
#define NAPT_PORT 10

#ifndef defaultAPSTASSID
#define defaultAPSTASSID  "ESP8266 extender" 
#define defaultAPPASSWORD "1234567890" 
#define defaultAPMAC      {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC}
#endif

#include <ESP8266WiFi.h>
#include <lwip/napt.h>
#include <lwip/dns.h>
#include <LwipDhcpServer.h>
#include <WiFiManager.h>         //https://github.com/tzapu/WiFiManager
//for LED status
#include <Ticker.h>
#include <EEPROM.h>
#if (ArduinoOTA_Enable == true)
  #include <ArduinoOTA.h>
#endif   


//for LED status
Ticker LED_ticker;
Ticker KEY_ticker;


bool shouldSaveConfig = false;
bool shouldReconfig = false;
bool shouldNAPTinit = false;
bool shouldOTArun = false;
bool WM_First_Run = true;

uint32_t KEY_Timer;
uint8_t KEY_Shut_Change_Timer;
uint64_t KEY_last_State_Change_tick;
char APSTASSID[64] = defaultAPSTASSID;
char APPASSWORD[64] = defaultAPPASSWORD;
char APMAC[18];
char APMAC_tmp[18];
uint8_t newMACAddress[] = defaultAPMAC;

WiFiManager wifiManager;
WiFiManagerParameter custom_apssid("APssid", "AP SSID", APSTASSID, 64);
WiFiManagerParameter custom_appsw("APpassword", "AP password", APPASSWORD, 64);
WiFiManagerParameter custom_apmac("Apmac", "AP MAC addr", APMAC, 18);


#if HAVE_NETDUMP
#include <NetDump.h>
void dump(int netif_idx, const char* data, size_t len, int out, int success) {
  (void)success;
  Serial.print(out ? F("out ") : F(" in "));
  Serial.printf("%d ", netif_idx);

  // optional filter example: if (netDump_is_ARP(data))
  {
    netDump(Serial, data, len);
    //netDumpHex(Serial, data, len);
  }
}
#endif

void LED_Tick_Service()
{
  //toggle state
  digitalWrite(LED_PIN, !digitalRead(LED_PIN));     // set pin to the opposite state
}


void KEY_Tick_Service(void)
{
  uint8_t Key_State_Now = digitalRead(KEY_PIN); 
  bool Key_State_Update = false;
  if(Key_State_Now == LOW)
  {
    KEY_Timer++;  
  }
  else
  {    
    if(KEY_Timer>5)
    {          
      Key_State_Update = true;        
    }
    KEY_Timer = 0;     
  }        
  if(Key_State_Update)
  {
    uint64_t Now_Tick =  millis(); 
    if(Now_Tick<KEY_last_State_Change_tick)
    {
      KEY_Shut_Change_Timer = 0;      
    }
    else
    {
      if((Now_Tick - KEY_last_State_Change_tick)<5000) //5秒内开关都算
      {
        KEY_Shut_Change_Timer++;         
        if(KEY_Shut_Change_Timer>5)//按五次重新配网
        {
          shouldReconfig = true;
          KEY_Shut_Change_Timer = 0;
        }
      } 
      else
      {
        KEY_Shut_Change_Timer = 0;  
      } 
    }  
    KEY_last_State_Change_tick = Now_Tick;  
  }
}

void KEY_Init(void)
{
  KEY_ticker.detach();
  KEY_last_State_Change_tick = millis();
  KEY_Shut_Change_Timer = 0;  
  KEY_Timer = 0; 
  KEY_ticker.attach_ms(5, KEY_Tick_Service);  
}

void MAC_Char2Str(char* MAC_Str,uint8_t* MAC_char)
{
  sprintf(MAC_Str,"%.2X:%.2X:%.2X:%.2X:%.2X:%.2X",MAC_char[0],MAC_char[1],MAC_char[2],MAC_char[3],MAC_char[4],MAC_char[5]);
}
byte nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';

    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;

    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return 16; // Not a valid hexadecimal character
}
bool MAC_Str2Char(uint8_t* MAC_char,char* MAC_Str)
{
  uint8_t mac_temp[6];
  uint8_t char_temp,currentByte,byteindex = 0;
  uint32_t i;
  for(i=0;i<17;i++)
  {
    if((i%3)==0)
    {
      char_temp = nibble(MAC_Str[i]);
      if(char_temp>15)return false;
      currentByte = char_temp << 4;
    }
    else if((i%3)==1)
    {
      char_temp = nibble(MAC_Str[i]);
      if(char_temp>15)return false;
      currentByte |= char_temp;
      mac_temp[byteindex] = currentByte;
      byteindex ++;
    }
    else
    {
      if(MAC_Str[i] != ':')return false;
    }
  }
  for(i=0;i<6;i++)MAC_char[i] = mac_temp[i];
  return true;
}

void EEPROM_SaveConfig()//保存函数
{
  uint8_t check_sum = 0;
 EEPROM.begin(256);//向系统申请256b ROM
 //开始写入
  uint8_t *p = (uint8_t*)(&APSTASSID);
  for (int i = 0; i < 64; i++)
  {
    EEPROM.write(i, p[i]); //在闪存内模拟写入
    check_sum += p[i];
  }
  p = (uint8_t*)(&APPASSWORD);
  for (int i = 0; i < 64; i++)
  {
    EEPROM.write(i+64, p[i]); //在闪存内模拟写入
    check_sum += p[i];
  }
  p = newMACAddress;
  for (int i = 0; i < 6; i++)
  {
    EEPROM.write(i+128, p[i]); //在闪存内模拟写入
    check_sum += p[i];
  }
  EEPROM.write(134, check_sum);
  EEPROM.end();//执行写入ROM
}

void EEPROM_ReadConfig()//保存函数
{
  uint8_t check_sum = 0;
  EEPROM.begin(256);//向系统申请4096b ROM
  //开始写入
  for (int i = 0; i < 134; i++)
  {
    check_sum += EEPROM.read(i);
  }
  if(EEPROM.read(134)!=check_sum)
  {
  #if (DBG_Printf_Enable == true)
    Serial.println("eeprom Reinit");
  #endif  
    EEPROM_SaveConfig();
  }
  else
  {
    uint8_t *p = (uint8_t*)(&APSTASSID);
    for (int i = 0; i < 64; i++)
    {
      p[i] = EEPROM.read(i+0x00);
      check_sum += p[i];
    }
    p = (uint8_t*)(&APPASSWORD);
    for (int i = 0; i < 64; i++)
    {
      p[i] = EEPROM.read(i+0x40);
      check_sum += p[i];
    }
    p = newMACAddress;
    for (int i = 0; i < 6; i++)
    {
      p[i] = EEPROM.read(i+0x80);
      check_sum += p[i];
    }
    #if (DBG_Printf_Enable == true)
      Serial.print("saved ap ssid: ");
      Serial.println(APSTASSID);
      Serial.print("saved ap psw: ");
      Serial.println(APSTASSID);
    #endif  
  }
  EEPROM.end();
  MAC_Char2Str(APMAC,newMACAddress);
  #if (DBG_Printf_Enable == true)
    Serial.print("saved ap mac: ");
    Serial.println(APMAC);
  #endif  
}

void WM_saveConfigCallback () 
{
  #if (DBG_Printf_Enable == true)
    Serial.println("save config");
  #endif   
  if(WiFi.status() == WL_CONNECTED)
  {
    #if (DBG_Printf_Enable == true)
      Serial.println("connected...yeey :)");  
    #endif  
    LED_ticker.detach();
    digitalWrite(LED_PIN, LED_ON);   
    //LED_ticker.attach(0.05, LED_Tick_Service);
    shouldSaveConfig = true;
    shouldNAPTinit = true;
  }
  else
  {
    #if (DBG_Printf_Enable == true)
      Serial.println("failed to connect and Configportal will run");  
    #endif  
    shouldReconfig = true;   
    shouldSaveConfig = false;
    shouldOTArun = false;   
  } 
}
void WM_saveParamsCallback () {
  #if (DBG_Printf_Enable == true)
    //Serial.println("save Params");
    Serial.println("Get Params:");
    Serial.print(custom_apssid.getID());Serial.print(" : ");Serial.println(custom_apssid.getValue());
    Serial.print(custom_appsw.getID());Serial.print(" : ");Serial.println(custom_appsw.getValue());
    Serial.print(custom_apmac.getID());Serial.print(" : ");Serial.println(custom_apmac.getValue());
  #endif
  strcpy(APMAC_tmp, custom_apmac.getValue());
  if(MAC_Str2Char(newMACAddress,APMAC_tmp))
  {
    strcpy(APSTASSID, custom_apssid.getValue());
    strcpy(APPASSWORD, custom_appsw.getValue());
    strcpy(APMAC, custom_apmac.getValue());
  }
  else
  {
  #if (DBG_Printf_Enable == true)
    Serial.println("Error Mac Input!!!!");
  #endif    
    shouldReconfig = true; 
    shouldSaveConfig = false;
    shouldOTArun = false;
    LED_ticker.attach(0.05, LED_Tick_Service);  
  }


}
void WM_ConfigPortalTimeoutCallback()
{
    shouldReconfig = true;   
    shouldSaveConfig = false;
    shouldOTArun = false;     
}

/**
 * 功能描述：初始化wifimanager
 */
void WifiManager_init()
{
  /***  explicitly set mode, esp defaults to STA+AP   **/
  WiFi.mode(WIFI_STA);
  //wifi_set_macaddr(STATION_IF, &newMACAddress[0]);//ST模式
  /*************************************/
  /*** 步骤二：进行一系列配置，参考配置类方法 **/
  // 配置连接超时
  wifiManager.setConnectTimeout(WifiManager_ConnectTimeout);
  wifiManager.setConfigPortalTimeout(WifiManager_ConfigPortalTimeout);
  // 打印调试内容    
  #if (DBG_Printf_Enable == true)
    wifiManager.setDebugOutput(true);
  #else  
     wifiManager.setDebugOutput(false);
  #endif
  
  // 设置最小信号强度
  wifiManager.setMinimumSignalQuality(30);
  // 设置固定AP信息
  IPAddress _ip = IPAddress(192, 168, 8, 8);
  IPAddress _gw = IPAddress(192, 168, 8, 1);
  IPAddress _sn = IPAddress(255, 255, 255, 0);
  wifiManager.setAPStaticIPConfig(_ip, _gw, _sn);
  // 设置点击保存的回调
  wifiManager.setSaveConfigCallback(WM_saveConfigCallback);
  wifiManager.setSaveParamsCallback(WM_saveParamsCallback);
  // 设置点击参赛复位的回调
  //wifiManager.setConfigResetCallback(WM_ConfigResetCallback);
  //面板超时
  wifiManager.setConfigPortalTimeoutCallback(WM_ConfigPortalTimeoutCallback);  
  // 设置 如果配置错误的ssid或者密码 退出配置模式
  wifiManager.setBreakAfterConfig(true);
  // 设置过滤重复的AP 默认可以不用调用 这里只是示范
  wifiManager.setRemoveDuplicateAPs(true);
  //非阻塞
  wifiManager.setConfigPortalBlocking(false);
  // 添加额外的参数 获取blinker的auth密钥 只运行一次 不然会加出很多框
  if(WM_First_Run)
  {
    custom_apssid.setValue(APSTASSID,16);
    custom_appsw.setValue(APPASSWORD,10);
    custom_apmac.setValue(APMAC,17);
    wifiManager.addParameter(&custom_apssid);   
    wifiManager.addParameter(&custom_appsw);
    wifiManager.addParameter(&custom_apmac); 
  }
  WM_First_Run = false;
 
  /*************************************/
  /*** 步骤三：尝试连接网络，失败去到配置页面 **/

    if (wifiManager.autoConnect()) 
    {
      #if (DBG_Printf_Enable == true)
        Serial.println("connected...yeey :)");  
      #endif  
      //LED_ticker.attach(0.05, LED_Tick_Service);
      LED_ticker.detach();
      digitalWrite(LED_PIN, LED_ON);
      shouldNAPTinit = true;
    }
    else
    {
      #if (DBG_Printf_Enable == true)
        Serial.println("failed to connect and Configportal running");  
      #endif  
      LED_ticker.attach(0.2, LED_Tick_Service);
      shouldNAPTinit = false;
    }
}

#if (ArduinoOTA_Enable == true)
  void ArduinoOTA_Init(void)
  {
    // Port defaults to 8266
    ArduinoOTA.setPort(8266);
    // Hostname defaults to esp8266-[ChipID]
    ArduinoOTA.setHostname("esp8266-extender");
    // No authentication by default
    ArduinoOTA.setPassword("posystorage3");
    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) {
          type = "sketch";
        } else { // U_FS
          type = "filesystem";
        }
  
        #if (DBG_Printf_Enable == true)
        // NOTE: if updating FS this would be the place to unmount FS using FS.end()
        Serial.println("Start updating " + type);
        #endif
      });
      ArduinoOTA.onEnd([]() {
        #if (DBG_Printf_Enable == true)
        Serial.println("\nEnd");
        #endif      
      });
      ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        #if (DBG_Printf_Enable == true)
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
        #endif      
      });
      ArduinoOTA.onError([](ota_error_t error) {
        #if (DBG_Printf_Enable == true)
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) {
          Serial.println("Auth Failed");
        } else if (error == OTA_BEGIN_ERROR) {
          Serial.println("Begin Failed");
        } else if (error == OTA_CONNECT_ERROR) {
          Serial.println("Connect Failed");
        } else if (error == OTA_RECEIVE_ERROR) {
          Serial.println("Receive Failed");
        } else if (error == OTA_END_ERROR) {
          Serial.println("End Failed");
        }
        #endif      
      });
      ArduinoOTA.begin();
  }
#endif   

void NAPT_Init(void)
{
#if (DBG_Printf_Enable == true)
  Serial.printf("\nSTA: %s (dns: %s / %s)\n",
                WiFi.localIP().toString().c_str(),
                WiFi.dnsIP(0).toString().c_str(),
                WiFi.dnsIP(1).toString().c_str());
#endif  
  
  wifi_set_macaddr(SOFTAP_IF, &newMACAddress[0]);//AP模式 
  // give DNS servers to AP side
  dhcpSoftAP.dhcps_set_dns(0, WiFi.dnsIP(0));
  dhcpSoftAP.dhcps_set_dns(1, WiFi.dnsIP(1));

  WiFi.softAPConfig(  // enable AP, with android-compatible google domain
    IPAddress(172, 217, 28, 254),
    IPAddress(172, 217, 28, 254),
    IPAddress(255, 255, 255, 0));
  WiFi.softAP(APSTASSID, APPASSWORD);
#if (DBG_Printf_Enable == true)
  Serial.printf("AP: %s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("Heap before: %d\n", ESP.getFreeHeap());
#endif 
  //修改AP的MAC地址
  //wifi_set_macaddr(STATION_IF, &newMACAddress[0]);//ST模式
  wifi_set_macaddr(SOFTAP_IF, &newMACAddress[0]);//AP模式 
 
#if (DBG_Printf_Enable == true) 
  Serial.print("mac:");               
  Serial.println(WiFi.macAddress()); 
#endif 

  err_t ret = ip_napt_init(NAPT, NAPT_PORT);
#if (DBG_Printf_Enable == true)
  Serial.printf("ip_napt_init(%d,%d): ret=%d (OK=%d)\n", NAPT, NAPT_PORT, (int)ret, (int)ERR_OK);
#endif 
  if (ret == ERR_OK) {
    ret = ip_napt_enable_no(SOFTAP_IF, 1);
#if (DBG_Printf_Enable == true)
    Serial.printf("ip_napt_enable_no(SOFTAP_IF): ret=%d (OK=%d)\n", (int)ret, (int)ERR_OK);
    if (ret == ERR_OK) {
      //Serial.printf("WiFi Network '%s' with same password is now NATed behind '%s'\n", STASSID "extender", STASSID);
    }
#endif 
  }
#if (DBG_Printf_Enable == true)
  Serial.printf("Heap after napt init: %d\n", ESP.getFreeHeap());
  if (ret != ERR_OK) {
    Serial.printf("NAPT initialization failed\n");
  }
#endif 
}

void setup(void) 
{
    #if (DBG_Printf_Enable == true)
    // 初始化串口
    Serial.begin(921600);
    Serial.println("");
    Serial.println("Serial 921600");
    Serial.printf("\n\nNAPT Range extender\n");
    Serial.printf("Heap on start: %d\n", ESP.getFreeHeap());
    #endif

#if HAVE_NETDUMP
    phy_capture = dump;
#endif    
            
    // 初始化有LED的IO
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LED_OFF);
    LED_ticker.attach(0.6, LED_Tick_Service);   
    //读取配置
    EEPROM_ReadConfig();  
    wifi_set_macaddr(SOFTAP_IF, &newMACAddress[0]);//AP模式 
    //开机一定时间后再具体初始化按键系统   
    KEY_Init();   
  // 重置保存的修改 目标是为了每次进来都是去掉配置页面
  //wifiManager.resetSettings();
    WM_First_Run = true;
    WifiManager_init();

  if(shouldNAPTinit)
  {
    NAPT_Init();
    #if (ArduinoOTA_Enable == true)
      ArduinoOTA_Init();
    #endif    
    shouldNAPTinit = false;
    shouldOTArun = true;
  } 
}


void loop(void) 
{
  wifiManager.process();  
  if(shouldReconfig)
  {
    shouldReconfig = false;
    shouldOTArun = false;
    wifiManager.resetSettings();
    delay(100);
    ESP.restart();
    //WifiManager_init();           
  }
  if(shouldSaveConfig)
  {
    shouldSaveConfig = false;    
    EEPROM_SaveConfig();
    delay(100);
    ESP.restart();
  } 
  if(shouldNAPTinit)
  {
    system_soft_wdt_feed();
    NAPT_Init();
    #if (ArduinoOTA_Enable == true)
      system_soft_wdt_feed();
      ArduinoOTA_Init();
    #endif  
    system_soft_wdt_feed();
    shouldNAPTinit = false;
    shouldOTArun = true;
  }  
  if(shouldOTArun)
  { 
    #if (ArduinoOTA_Enable == true)
      ArduinoOTA.handle(); 
    #endif  
  } 
}

