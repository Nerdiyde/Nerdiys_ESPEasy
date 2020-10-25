#define WIFI_RECONNECT_WAIT         20000 // in milliSeconds
#define WIFI_AP_OFF_TIMER_DURATION  60000 // in milliSeconds

#ifdef CORE_POST_2_5_0
# include <AddrList.h>
#endif // ifdef CORE_POST_2_5_0

bool unprocessedWifiEvents() {
  if (processedConnect && processedGotIP && processedDisconnect) { return false; }
  return true;
}

// ********************************************************************************
// Functions to process the data gathered from the events.
// These functions are called from Setup() or Loop() and thus may call delay() or yield()
// ********************************************************************************
void processConnect() {
  if (processedConnect) { return; }
  processedConnect = true;
  wifiStatus |= ESPEASY_WIFI_CONNECTED;
  delay(100); // FIXME TD-er: See https://github.com/letscontrolit/ESPEasy/issues/1987#issuecomment-451644424
  ++wifi_reconnects;

  if (wifiStatus < ESPEASY_WIFI_CONNECTED) { return; }
  const long connect_duration = timeDiff(last_wifi_connect_attempt_moment, lastConnectMoment);

  if (loglevelActiveFor(LOG_LEVEL_INFO)) {
    String log = F("WIFI : Connected! AP: ");
    log += WiFi.SSID();
    log += " (";
    log += WiFi.BSSIDstr();
    log += F(") Ch: ");
    log += last_channel;

    if ((connect_duration > 0) && (connect_duration < 30000)) {
      // Just log times when they make sense.
      log += F(" Duration: ");
      log += connect_duration;
      log += F(" ms");
    }
    addLog(LOG_LEVEL_INFO, log);
  }

  if (Settings.UseRules && bssid_changed) {
    String event = F("WiFi#ChangedAccesspoint");
    rulesProcessing(event);
  }

  if (useStaticIP()) {
    markGotIP(); // in static IP config the got IP event is never fired.
  }

  if (!WiFi.getAutoConnect()) {
    WiFi.setAutoConnect(true);
  }
  logConnectionStatus();
}

void processDisconnect() {
  if (processedDisconnect) { return; }
  processedDisconnect = true;
  wifiStatus = ESPEASY_WIFI_DISCONNECTED;
  delay(100); // FIXME TD-er: See https://github.com/letscontrolit/ESPEasy/issues/1987#issuecomment-451644424

  if (Settings.UseRules) {
    String event = F("WiFi#Disconnected");
    rulesProcessing(event);
  }

  if (loglevelActiveFor(LOG_LEVEL_INFO)) {
    String log = F("WIFI : Disconnected! Reason: '");
    log += getLastDisconnectReason();
    log += '\'';

    if (lastConnectedDuration > 0) {
      log += F(" Connected for ");
      log += format_msec_duration(lastConnectedDuration);
    }
    addLog(LOG_LEVEL_INFO, log);
  }

  if (Settings.WiFiRestart_connection_lost()) {
    setWifiMode(WIFI_OFF);
    delay(100);
  }
  logConnectionStatus();
}

void processGotIP() {
  if (processedGotIP) {
    return;
  }
  IPAddress ip = WiFi.localIP();

  if (!useStaticIP()) {
    if ((ip[0] == 0) && (ip[1] == 0) && (ip[2] == 0) && (ip[3] == 0)) {
      return;
    }
  }
  processedGotIP = true;
  wifiStatus |= ESPEASY_WIFI_GOT_IP;
  const IPAddress gw       = WiFi.gatewayIP();
  const IPAddress subnet   = WiFi.subnetMask();
  const long dhcp_duration = timeDiff(lastConnectMoment, lastGetIPmoment);

  if (loglevelActiveFor(LOG_LEVEL_INFO)) {
    String log = F("WIFI : ");

    if (useStaticIP()) {
      log += F("Static IP: ");
    } else {
      log += F("DHCP IP: ");
    }
    log += formatIP(ip);
    log += " (";
    log += WifiGetHostname();
    log += F(") GW: ");
    log += formatIP(gw);
    log += F(" SN: ");
    log += formatIP(subnet);

    if ((dhcp_duration > 0) && (dhcp_duration < 30000)) {
      // Just log times when they make sense.
      log += F("   duration: ");
      log += dhcp_duration;
      log += F(" ms");
    }
    addLog(LOG_LEVEL_INFO, log);
  }

  // Might not work in core 2.5.0
  // See https://github.com/esp8266/Arduino/issues/5839
  if ((Settings.IP_Octet != 0) && (Settings.IP_Octet != 255))
  {
    ip[3] = Settings.IP_Octet;

    if (loglevelActiveFor(LOG_LEVEL_INFO)) {
      String log = F("IP   : Fixed IP octet:");
      log += formatIP(ip);
      addLog(LOG_LEVEL_INFO, log);
    }
    WiFi.config(ip, gw, subnet);
  }

  #ifdef FEATURE_MDNS
  addLog(LOG_LEVEL_INFO, F("WIFI : Starting mDNS..."));
  bool mdns_started = MDNS.begin(WifiGetHostname().c_str());

  if (loglevelActiveFor(LOG_LEVEL_INFO)) {
    String log = F("WIFI : ");

    if (mdns_started) {
      log += F("mDNS started, with name: ");
      log += WifiGetHostname();
      log += F(".local");
    }
    else {
      log += F("mDNS failed");
    }
    addLog(LOG_LEVEL_INFO, log);
  }
  #endif // ifdef FEATURE_MDNS

  // First try to get the time, since that may be used in logs
  if (systemTimePresent()) {
    initTime();
  }
  mqtt_reconnect_count        = 0;
  MQTTclient_should_reconnect = true;
  timermqtt_interval          = 100;
  setIntervalTimer(TIMER_MQTT);
  sendGratuitousARP_now();

  if (Settings.UseRules)
  {
    String event = F("WiFi#Connected");
    rulesProcessing(event);
  }
  statusLED(true);

  //  WiFi.scanDelete();
  setWebserverRunning(true);
  #ifdef FEATURE_MDNS

  if (mdns_started) {
    MDNS.addService("http", "tcp", 80);
  }
  #endif // ifdef FEATURE_MDNS
  wifi_connect_attempt = 0;

  if (wifiSetup) {
    // Wifi setup was active, Apparently these settings work.
    wifiSetup = false;
    SaveSettings();
  }
  logConnectionStatus();
}

void processConnectAPmode() {
  if (processedConnectAPmode) { return; }
  processedConnectAPmode = true;

  if (loglevelActiveFor(LOG_LEVEL_INFO)) {
    String log = F("AP Mode: Client connected: ");
    log += formatMAC(lastMacConnectedAPmode);
    log += F(" Connected devices: ");
    log += WiFi.softAPgetStationNum();
    addLog(LOG_LEVEL_INFO, log);
  }
  timerAPoff = 0; // Disable timer to switch AP off
  setWebserverRunning(true);

  // Start DNS, only used if the ESP has no valid WiFi config
  // It will reply with it's own address on all DNS requests
  // (captive portal concept)
  if (!dnsServerActive) {
    dnsServerActive = true;
    dnsServer.start(DNS_PORT, "*", apIP);
  }
}

void processDisconnectAPmode() {
  if (processedDisconnectAPmode) { return; }
  processedDisconnectAPmode = true;
  const int nrStationsConnected = WiFi.softAPgetStationNum();

  if (loglevelActiveFor(LOG_LEVEL_INFO)) {
    String log = F("AP Mode: Client disconnected: ");
    log += formatMAC(lastMacDisconnectedAPmode);
    log += F(" Connected devices: ");
    log += nrStationsConnected;
    addLog(LOG_LEVEL_INFO, log);
  }

  if (nrStationsConnected == 0) {
    timerAPoff = millis() + WIFI_AP_OFF_TIMER_DURATION;
  }
}

void processDisableAPmode() {
  if (timerAPoff == 0) { return; }

  if (WifiIsAP(WiFi.getMode())) {
    // disable AP after timeout.
    if (timeOutReached(timerAPoff)) {
      setAP(false);
    }
  }
}

void processScanDone() {
  if (processedScanDone) { return; }
  processedScanDone = true;

  if (loglevelActiveFor(LOG_LEVEL_INFO)) {
    String log = F("WIFI  : Scan finished, found: ");
    log += scan_done_number;
    addLog(LOG_LEVEL_INFO, log);
  }

  int bestScanID           = -1;
  int32_t bestRssi         = -1000;
  uint8_t bestWiFiSettings = lastWiFiSettings;

  if (selectValidWiFiSettings()) {
    bool   done                 = false;
    String lastWiFiSettingsSSID = getLastWiFiSettingsSSID();

    for (int settingNr = 0; !done && settingNr < 2; ++settingNr) {
      for (int i = 0; i < scan_done_number; ++i) {
        if (WiFi.SSID(i) == lastWiFiSettingsSSID) {
          int32_t rssi = WiFi.RSSI(i);

          if (bestRssi < rssi) {
            bestRssi         = rssi;
            bestScanID       = i;
            bestWiFiSettings = lastWiFiSettings;
          }
        }
      }

      if (!selectNextWiFiSettings()) { done = true; }
    }

    if (bestScanID >= 0) {
      if (loglevelActiveFor(LOG_LEVEL_INFO)) {
        String log = F("WIFI  : Selected: ");
        log += formatScanResult(bestScanID, " ");
        addLog(LOG_LEVEL_INFO, log);
      }
      lastWiFiSettings = bestWiFiSettings;
      uint8_t *scanbssid = WiFi.BSSID(bestScanID);

      if (scanbssid) {
        for (int i = 0; i < 6; ++i) {
          lastBSSID[i] = *(scanbssid + i);
        }
      }
    }
  }
}

void resetWiFi() {
  addLog(LOG_LEVEL_INFO, F("Reset WiFi."));
  lastDisconnectMoment = millis();
  // Mark all flags to default to prevent handling old events.
  processedConnect = true;
  processedDisconnect = true;
  processedGotIP = true;
  processedDHCPTimeout = true;
  processedConnectAPmode = true;
  processedDisconnectAPmode = true;
  processedScanDone = true;
  WifiDisconnect();

  //  setWifiMode(WIFI_OFF);

#ifdef ESP8266

  // See https://github.com/esp8266/Arduino/issues/5527#issuecomment-460537616
  WiFi.~ESP8266WiFiClass();
  WiFi = ESP8266WiFiClass();
#endif // ifdef ESP8266
}

bool hasIPaddr() {
#ifdef CORE_POST_2_5_0
  bool configured = false;

  for (auto addr : addrList) {
    if ((configured = !addr.isLocal() && (addr.ifnumber() == STATION_IF))) {
      /*
         Serial.printf("STA: IF='%s' hostname='%s' addr= %s\n",
                    addr.ifname().c_str(),
                    addr.ifhostname(),
                    addr.toString().c_str());
       */
      break;
    }
  }
  return configured;
#else // ifdef CORE_POST_2_5_0
  return WiFi.isConnected();
#endif // ifdef CORE_POST_2_5_0
}

void connectionCheckHandler()
{
  if ((reconnectChecker == false) && !WiFiConnected()) {
    reconnectChecker = true;
    resetWiFi();
    WiFi.reconnect();
  }
  else if (WiFiConnected() && (reconnectChecker == true)) {
    reconnectChecker = false;
  }
}

void WifiScanAsync() {
  addLog(LOG_LEVEL_INFO, F("WIFI  : Start network scan"));
  #ifdef ESP32
  bool async               = true;
  bool show_hidden         = false;
  bool passive             = false;
  uint32_t max_ms_per_chan = 300;
  WiFi.scanNetworks(async, show_hidden, passive, max_ms_per_chan);
  #else // ifdef ESP32

  // 2.4.x only and it doesn't work like expected.
  //    WiFi.scanNetworksAsync(onScanFinished);
  #endif // ifdef ESP32
}

// ********************************************************************************
// WiFi state
// ********************************************************************************
// 0  Wifi off
// 1  Start                        => STA
// 2  Try connect                  => STA      => successful, goto 7
// 3  Connection fails             => AP       => no client in N seconds, goto 1
// 4  Client connects              => AP
// 5  Credentials changed          => STA_AP   => goto 2
// 6  Connect successful (AP on)   => STA_AP   => disable AP after N seconds, goto 8
// 7  Connect successful           => STA
// 8  Disable AP after N seconds   => STA
// ********************************************************************************

bool WifiIsAP(WiFiMode_t wifimode)
{
  #if defined(ESP32)
  return (wifimode == WIFI_MODE_AP) || (wifimode == WIFI_MODE_APSTA);
  #else // if defined(ESP32)
  return (wifimode == WIFI_AP) || (wifimode == WIFI_AP_STA);
  #endif // if defined(ESP32)
}

bool WifiIsSTA(WiFiMode_t wifimode)
{
  #if defined(ESP32)
  return (wifimode & WIFI_MODE_STA) != 0;
  #else // if defined(ESP32)
  return (wifimode & WIFI_STA) != 0;
  #endif // if defined(ESP32)
}

// ********************************************************************************
// Set Wifi Mode
// ********************************************************************************
void setSTA(bool enable) {
  switch (WiFi.getMode()) {
    case WIFI_OFF:

      if (enable) { setWifiMode(WIFI_STA); }
      break;
    case WIFI_STA:

      if (!enable) { setWifiMode(WIFI_OFF); }
      break;
    case WIFI_AP:

      if (enable) { setWifiMode(WIFI_AP_STA); }
      break;
    case WIFI_AP_STA:

      if (!enable) { setWifiMode(WIFI_AP); }
      break;
    default:
      break;
  }
}

void setAP(bool enable) {
  WiFiMode_t wifimode = WiFi.getMode();

  switch (wifimode) {
    case WIFI_OFF:

      if (enable) { setWifiMode(WIFI_AP); }
      break;
    case WIFI_STA:

      if (enable) { setWifiMode(WIFI_AP_STA); }
      break;
    case WIFI_AP:

      if (!enable) { setWifiMode(WIFI_OFF); }
      break;
    case WIFI_AP_STA:

      if (!enable) { setWifiMode(WIFI_STA); }
      break;
    default:
      break;
  }
}

// Only internal scope
void setAPinternal(bool enable)
{
  if (enable) {
    // create and store unique AP SSID/PW to prevent ESP from starting AP mode with default SSID and No password!
    // setup ssid for AP Mode when needed
    String softAPSSID = WifiGetAPssid();
    String pwd        = SecuritySettings.WifiAPKey;
    IPAddress subnet(DEFAULT_AP_SUBNET);

    if (!WiFi.softAPConfig(apIP, apIP, subnet)) {
      addLog(LOG_LEVEL_ERROR, F("WIFI : [AP] softAPConfig failed!"));
    }

    if (WiFi.softAP(softAPSSID.c_str(), pwd.c_str())) {
      if (loglevelActiveFor(LOG_LEVEL_INFO)) {
        String event = F("WiFi#APmodeEnabled");
        rulesProcessing(event);
        String log(F("WIFI : AP Mode ssid will be "));
        log += softAPSSID;
        log += F(" with address ");
        log += WiFi.softAPIP().toString();
        addLog(LOG_LEVEL_INFO, log);
      }
    } else {
      if (loglevelActiveFor(LOG_LEVEL_ERROR)) {
        String log(F("WIFI : Error while starting AP Mode with SSID: "));
        log += softAPSSID;
        log += F(" IP: ");
        log += apIP.toString();
        addLog(LOG_LEVEL_ERROR, log);
      }
    }
    #ifdef ESP32

    #else // ifdef ESP32

    if (wifi_softap_dhcps_status() != DHCP_STARTED) {
      if (!wifi_softap_dhcps_start()) {
        addLog(LOG_LEVEL_ERROR, F("WIFI : [AP] wifi_softap_dhcps_start failed!"));
      }
    }
    #endif // ifdef ESP32
  } else {
    timerAPoff = 0; // Disable timer to switch AP off

    if (dnsServerActive) {
      dnsServerActive = false;
      dnsServer.stop();
    }
  }
}

void setWifiMode(WiFiMode_t wifimode) {
  const WiFiMode_t cur_mode = WiFi.getMode();

  if (cur_mode == wifimode) {
    return;
  }

  if (wifimode != WIFI_OFF) {
    #ifdef ESP8266

    // See: https://github.com/esp8266/Arduino/issues/6172#issuecomment-500457407
    WiFi.forceSleepWake(); // Make sure WiFi is really active.
    #endif // ifdef ESP8266
    delay(100);
  }

  switch (wifimode) {
    case WIFI_OFF:
      addLog(LOG_LEVEL_INFO, F("WIFI : Switch off WiFi"));
      break;
    case WIFI_STA:
      addLog(LOG_LEVEL_INFO, F("WIFI : Set WiFi to STA"));
      break;
    case WIFI_AP:
      addLog(LOG_LEVEL_INFO, F("WIFI : Set WiFi to AP"));
      break;
    case WIFI_AP_STA:
      addLog(LOG_LEVEL_INFO, F("WIFI : Set WiFi to AP+STA"));
      break;
    default:
      addLog(LOG_LEVEL_INFO, F("WIFI : Unknown mode"));
      break;
  }

  if (!WiFi.mode(wifimode)) {
    addLog(LOG_LEVEL_INFO, F("WIFI : Cannot set mode!!!!!"));
  }

  if (wifimode == WIFI_OFF) {
    delay(1000);
    #ifdef ESP8266
    WiFi.forceSleepBegin();
    #endif
    delay(1);
  } else {
    delay(30); // Must allow for some time to init.
  }
  bool new_mode_AP_enabled = WifiIsAP(wifimode);

  if (WifiIsAP(cur_mode) && !new_mode_AP_enabled) {
    String event = F("WiFi#APmodeDisabled");
    rulesProcessing(event);
  }

  if (WifiIsAP(cur_mode) != new_mode_AP_enabled) {
    // Mode has changed
    setAPinternal(new_mode_AP_enabled);
  }
}

// ********************************************************************************
// Determine Wifi AP name to set. (also used for mDNS)
// ********************************************************************************
String WifiGetAPssid()
{
  String ssid(Settings.Name);

  if (Settings.appendUnitToHostname()) {
    ssid += "_";
    ssid += Settings.Unit;
  }
  return ssid;
}

// ********************************************************************************
// Determine hostname: basically WifiGetAPssid with spaces changed to -
// ********************************************************************************
String WifiGetHostname()
{
  String hostname(WifiGetAPssid());

  hostname.replace(" ", "-");
  hostname.replace("_", "-"); // See RFC952
  return hostname;
}

bool useStaticIP() {
  return Settings.IP[0] != 0 && Settings.IP[0] != 255;
}

bool WiFiConnected() {
  START_TIMER;

  if (unprocessedWifiEvents()) { return false; }

  // For ESP82xx, do not rely on WiFi.status() with event based wifi.
  if (wifiStatus == ESPEASY_WIFI_SERVICES_INITIALIZED) {
    if ((WiFi.RSSI() < 0) && WiFi.isConnected()) {
      timerAPstart = 0;
      processDisableAPmode();
      STOP_TIMER(WIFI_ISCONNECTED_STATS);
      return true;
    }

    // else wifiStatus is no longer in sync.
    addLog(LOG_LEVEL_INFO, F("WIFI  : WiFiConnected() out of sync"));
    resetWiFi();
  } else {
    if (hasIPaddr() && WiFi.isConnected()) {
      wifiStatus = ESPEASY_WIFI_SERVICES_INITIALIZED;
    }
  }

  if (timerAPstart == 0) {
    timerAPstart = millis() + WIFI_RECONNECT_WAIT;
  }

  if (timeOutReached(timerAPstart)) {
    setAP(true);
  }
  delay(1);
  STOP_TIMER(WIFI_NOTCONNECTED_STATS);
  return false;
}

void WiFiConnectRelaxed() {
  if (WiFiConnected()) {
    return; // already connected, need to disconnect first
  }

  if (prepareWiFi()) {
    if (selectValidWiFiSettings()) {
      tryConnectWiFi();
      return;
    }
  }
  addLog(LOG_LEVEL_ERROR, F("WIFI : Could not connect to AP!"));

  // everything failed, activate AP mode (will deactivate automatically after a while if its connected again)
  setAP(true);
}

// ********************************************************************************
// Set Wifi config
// ********************************************************************************
bool prepareWiFi() {
  if (!selectValidWiFiSettings()) {
    addLog(LOG_LEVEL_ERROR, F("WIFI : No valid wifi settings"));
    return false;
  }
  setSTA(true);
  char hostname[40];
  safe_strncpy(hostname, WifiGetHostname().c_str(), sizeof(hostname));
  #if defined(ESP8266)
  wifi_station_set_hostname(hostname);

  if (Settings.WifiNoneSleep()) {
    // Only set this mode during setup.
    // Reset to default power mode requires a reboot since setting it to WIFI_LIGHT_SLEEP will cause a crash.
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
  }
  #endif // if defined(ESP8266)
  #if defined(ESP32)
  WiFi.setHostname(hostname);
  #endif // if defined(ESP32)
  return true;
}

// ********************************************************************************
// Manage WiFi credentials
// ********************************************************************************
const char* getLastWiFiSettingsSSID() {
  return lastWiFiSettings == 0 ? SecuritySettings.WifiSSID : SecuritySettings.WifiSSID2;
}

const char* getLastWiFiSettingsPassphrase() {
  return lastWiFiSettings == 0 ? SecuritySettings.WifiKey : SecuritySettings.WifiKey2;
}

bool selectNextWiFiSettings() {
  uint8_t tmp = lastWiFiSettings;

  lastWiFiSettings = (lastWiFiSettings + 1) % 2;

  if (!wifiSettingsValid(getLastWiFiSettingsSSID(), getLastWiFiSettingsPassphrase())) {
    // other settings are not correct, switch back.
    lastWiFiSettings = tmp;
    return false; // Nothing changed.
  }
  return true;
}

bool selectValidWiFiSettings() {
  if (wifiSettingsValid(getLastWiFiSettingsSSID(), getLastWiFiSettingsPassphrase())) {
    return true;
  }
  return selectNextWiFiSettings();
}

bool wifiSettingsValid(const char *ssid, const char *pass) {
  if ((ssid[0] == 0) || (strcasecmp(ssid, "ssid") == 0)) {
    return false;
  }

  //  if (pass[0] == 0) return false; // Allow for empty pass
  if (strlen(ssid) > 32) { return false; }

  if (strlen(pass) > 64) { return false; }
  return true;
}

bool wifiConnectTimeoutReached() {
  if (wifi_connect_attempt == 0) { return true; }

  if (timeDiff(last_wifi_connect_attempt_moment, lastDisconnectMoment) > 0) {
    // Connection attempt was already ended.
    return true;
  }

  if (WifiIsAP(WiFi.getMode())) {
    // Initial setup of WiFi, may take much longer since accesspoint is still active.
    return timeOutReached(last_wifi_connect_attempt_moment + 20000);
  }

  // wait until it connects + add some device specific random offset to prevent
  // all nodes overloading the accesspoint when turning on at the same time.
  #if defined(ESP8266)
  const unsigned int randomOffset_in_sec = (wifi_connect_attempt == 1) ? 0 : 1000 * ((ESP.getChipId() & 0xF));
  #endif // if defined(ESP8266)
  #if defined(ESP32)
  const unsigned int randomOffset_in_sec = (wifi_connect_attempt == 1) ? 0 : 1000 * ((ESP.getEfuseMac() & 0xF));
  #endif // if defined(ESP32)
  return timeOutReached(last_wifi_connect_attempt_moment + DEFAULT_WIFI_CONNECTION_TIMEOUT + randomOffset_in_sec);
}

void setConnectionSpeed() {
  #ifdef ESP8266

  if (!Settings.ForceWiFi_bg_mode() || (wifi_connect_attempt > 10)) {
    WiFi.setPhyMode(WIFI_PHY_MODE_11N);
  } else {
    WiFi.setPhyMode(WIFI_PHY_MODE_11G);
  }
  #endif // ifdef ESP8266

  // Does not (yet) work, so commented out.
  #ifdef ESP32
  uint8_t protocol = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G; // Default to BG

  if (!Settings.ForceWiFi_bg_mode() || (wifi_connect_attempt > 10)) {
    // Set to use BGN
    protocol |= WIFI_PROTOCOL_11N;
  }

  if (WifiIsSTA(WiFi.getMode())) {
    esp_wifi_set_protocol(WIFI_IF_STA, protocol);
  }

  if (WifiIsAP(WiFi.getMode())) {
    esp_wifi_set_protocol(WIFI_IF_AP, protocol);
  }
  #endif // ifdef ESP32
}

void setupStaticIPconfig() {
  setUseStaticIP(useStaticIP());

  if (!useStaticIP()) { return; }
  const IPAddress ip     = Settings.IP;
  const IPAddress gw     = Settings.Gateway;
  const IPAddress subnet = Settings.Subnet;
  const IPAddress dns    = Settings.DNS;

  if (loglevelActiveFor(LOG_LEVEL_INFO)) {
    String log = F("IP   : Static IP : ");
    log += formatIP(ip);
    log += F(" GW: ");
    log += formatIP(gw);
    log += F(" SN: ");
    log += formatIP(subnet);
    log += F(" DNS: ");
    log += formatIP(dns);
    addLog(LOG_LEVEL_INFO, log);
  }
  WiFi.config(ip, gw, subnet, dns);
}

// ********************************************************************************
// Simply start the WiFi connection sequence
// ********************************************************************************
bool tryConnectWiFi() {
  if (wifiSetupConnect) {
    lastWiFiSettings     = 0; // Force to load the first settings.
    wifi_connect_attempt = 0;
  }

  if (wifiStatus != ESPEASY_WIFI_DISCONNECTED) {
    if (!WifiIsAP(WiFi.getMode())) {
      // Only when not in AP mode.
      return true; // already connected, need to disconnect first
    }
  }

  if (!wifiConnectTimeoutReached()) {
    delay(0);    // SMY: give some time to handle WiFi
    return true; // timeout not reached yet, thus no need to retry again.
  }

  if (!selectValidWiFiSettings()) {
    addLog(LOG_LEVEL_ERROR, F("WIFI : No valid WiFi settings!"));
    return false;
  }

  if ((wifi_connect_attempt != 0) && ((wifi_connect_attempt % 2) == 0)) {
    selectNextWiFiSettings();
  }

  if (wifi_connect_attempt > 5) {
    setAP(true);
  }
  const char *ssid       = getLastWiFiSettingsSSID();
  const char *passphrase = getLastWiFiSettingsPassphrase();

  if (loglevelActiveFor(LOG_LEVEL_INFO)) {
    String log = F("WIFI : Connecting ");
    log += ssid;
    log += F(" attempt #");
    log += wifi_connect_attempt;
    addLog(LOG_LEVEL_INFO, log);
  }
  setupStaticIPconfig();
  setConnectionSpeed();
  last_wifi_connect_attempt_moment = millis();

  switch (wifi_connect_attempt) {
    case 0:

      if (lastBSSID[0] == 0) {
        WiFi.begin(ssid, passphrase);
      }
      else {
        WiFi.begin(ssid, passphrase, last_channel, &lastBSSID[0]);
      }
      break;
    default:
      WiFi.begin(ssid, passphrase);
  }
  ++wifi_connect_attempt;
  logConnectionStatus();

  switch (WiFi.status()) {
    case WL_NO_SSID_AVAIL: {
      if (loglevelActiveFor(LOG_LEVEL_INFO)) {
        String log = F("WIFI : No SSID found matching: ");
        log += ssid;
        addLog(LOG_LEVEL_INFO, log);
      }
      break;
    }
    case WL_CONNECT_FAILED: {
      if (loglevelActiveFor(LOG_LEVEL_INFO)) {
        String log = F("WIFI : Connection failed to: ");
        log += ssid;
        addLog(LOG_LEVEL_INFO, log);
      }
      break;
    }
    case WL_DISCONNECTED: {
      if (loglevelActiveFor(LOG_LEVEL_INFO)) {
        String log = F("WIFI : WiFi.status() = WL_DISCONNECTED  SSID: ");
        log += ssid;
        addLog(LOG_LEVEL_INFO, log);
      }
      break;
    }
    case WL_IDLE_STATUS: {
      if (loglevelActiveFor(LOG_LEVEL_INFO)) {
        String log = F("WIFI : Connection in IDLE state: ");
        log += ssid;
        addLog(LOG_LEVEL_INFO, log);
      }
      break;
    }
    case WL_CONNECTED: {
      return true;
    }
    default:
      break;
  }
  return false;
}

// ********************************************************************************
// Disconnect from Wifi AP
// ********************************************************************************
void WifiDisconnect()
{
  #if defined(ESP32)
  WiFi.disconnect();
  #else // if defined(ESP32)
  ETS_UART_INTR_DISABLE();
  wifi_station_disconnect();
  ETS_UART_INTR_ENABLE();
  #endif // if defined(ESP32)
  wifiStatus          = ESPEASY_WIFI_DISCONNECTED;
  processedDisconnect = false;
}

// ********************************************************************************
// Scan all Wifi Access Points
// ********************************************************************************
void WifiScan()
{
  // Direct Serial is allowed here, since this function will only be called from serial input.
  serialPrintln(F("WIFI : SSID Scan start"));
  int n = WiFi.scanNetworks(false, true);

  if (n == 0) {
    serialPrintln(F("WIFI : No networks found"));
  }
  else
  {
    serialPrint(     F("WIFI : "));
    serialPrint(String(n));
    serialPrintln(F(" networks found"));

    for (int i = 0; i < n; ++i)
    {
      // Print SSID and RSSI for each network found
      serialPrint(     F("WIFI : "));
      serialPrint(String(i + 1));
      serialPrint(  ": ");
      serialPrintln(formatScanResult(i, " "));
      delay(10);
    }
  }
  serialPrintln("");
}

String formatScanResult(int i, const String& separator) {
  String result = WiFi.SSID(i);

  htmlEscape(result);
  #ifndef ESP32

  if (WiFi.isHidden(i)) {
    result += F("#Hidden#");
  }
  #endif // ifndef ESP32
  result += separator;
  result += WiFi.BSSIDstr(i);
  result += separator;
  result += F("Ch:");
  result += WiFi.channel(i);
  result += " (";
  result += WiFi.RSSI(i);
  result += F("dBm) ");

  switch (WiFi.encryptionType(i)) {
  #ifdef ESP32
    case WIFI_AUTH_OPEN: result            += F("open"); break;
    case WIFI_AUTH_WEP:  result            += F("WEP"); break;
    case WIFI_AUTH_WPA_PSK: result         += F("WPA/PSK"); break;
    case WIFI_AUTH_WPA2_PSK: result        += F("WPA2/PSK"); break;
    case WIFI_AUTH_WPA_WPA2_PSK: result    += F("WPA/WPA2/PSK"); break;
    case WIFI_AUTH_WPA2_ENTERPRISE: result += F("WPA2 Enterprise"); break;
  #else // ifdef ESP32
    case ENC_TYPE_WEP: result  += F("WEP"); break;
    case ENC_TYPE_TKIP: result += F("WPA/PSK"); break;
    case ENC_TYPE_CCMP: result += F("WPA2/PSK"); break;
    case ENC_TYPE_NONE: result += F("open"); break;
    case ENC_TYPE_AUTO: result += F("WPA/WPA2/PSK"); break;
  #endif // ifdef ESP32
    default:
      break;
  }
  return result;
}

#ifndef ESP32
String SDKwifiStatusToString(uint8_t sdk_wifistatus) {
  switch (sdk_wifistatus) {
    case STATION_IDLE:           return F("STATION_IDLE");
    case STATION_CONNECTING:     return F("STATION_CONNECTING");
    case STATION_WRONG_PASSWORD: return F("STATION_WRONG_PASSWORD");
    case STATION_NO_AP_FOUND:    return F("STATION_NO_AP_FOUND");
    case STATION_CONNECT_FAIL:   return F("STATION_CONNECT_FAIL");
    case STATION_GOT_IP:         return F("STATION_GOT_IP");
  }
  return getUnknownString();
}

#endif // ifndef ESP32

String ArduinoWifiStatusToString(uint8_t arduino_corelib_wifistatus) {
  String log;

  switch (arduino_corelib_wifistatus) {
    case WL_IDLE_STATUS:     log += F("WL_IDLE_STATUS"); break;
    case WL_NO_SSID_AVAIL:   log += F("WL_NO_SSID_AVAIL"); break;
    case WL_SCAN_COMPLETED:  log += F("WL_SCAN_COMPLETED"); break;
    case WL_CONNECTED:       log += F("WL_CONNECTED"); break;
    case WL_CONNECT_FAILED:  log += F("WL_CONNECT_FAILED"); break;
    case WL_CONNECTION_LOST: log += F("WL_CONNECTION_LOST"); break;
    case WL_DISCONNECTED:    log += F("WL_DISCONNECTED"); break;
    default:  log                += arduino_corelib_wifistatus; break;
  }
  return log;
}

String ESPeasyWifiStatusToString() {
  String log;

  switch (wifiStatus) {
    case ESPEASY_WIFI_DISCONNECTED:         log += F("ESPEASY_WIFI_DISCONNECTED"); break;
    case ESPEASY_WIFI_CONNECTED:            log += F("ESPEASY_WIFI_CONNECTED"); break;
    case ESPEASY_WIFI_GOT_IP:               log += F("ESPEASY_WIFI_GOT_IP"); break;
    case ESPEASY_WIFI_SERVICES_INITIALIZED: log += F("ESPEASY_WIFI_SERVICES_INITIALIZED"); break;
    default:  log                               += wifiStatus;
  }
  return log;
}

void logConnectionStatus() {
  String log;

  #ifndef ESP32
  const uint8_t arduino_corelib_wifistatus = WiFi.status();
  const uint8_t sdk_wifistatus             = wifi_station_get_connect_status();

  if ((arduino_corelib_wifistatus == WL_CONNECTED) != (sdk_wifistatus == STATION_GOT_IP)) {
    if (loglevelActiveFor(LOG_LEVEL_ERROR)) {
      String log = F("WIFI  : SDK station status differs from Arduino status. SDK-status: ");
      log += SDKwifiStatusToString(sdk_wifistatus);
      log += F(" Arduino status: ");
      log += ArduinoWifiStatusToString(arduino_corelib_wifistatus);
      addLog(LOG_LEVEL_ERROR, log);
    }
  }
  #endif // ifndef ESP32
#ifndef BUILD_NO_DEBUG

  if (loglevelActiveFor(LOG_LEVEL_DEBUG_MORE)) {
    String log = F("WIFI  : Arduino wifi status: ");
    log += ArduinoWifiStatusToString(WiFi.status());
    log += F(" ESPeasy internal wifi status: ");
    log += ESPeasyWifiStatusToString();
    addLog(LOG_LEVEL_DEBUG_MORE, log);
  }
#endif // ifndef BUILD_NO_DEBUG
}

// ********************************************************************************
// Check if we are still connected to a Wifi AP
// ********************************************************************************
void WifiCheck()
{
  if (wifiSetup) {
    return;
  }

  processDisableAPmode();
  IPAddress ip = WiFi.localIP();

  if (WiFiConnected()) { // let's just check for wifi, no matter if we have an IP or not...
    if (!useStaticIP()) {
      if ((ip[0] == 0) && (ip[1] == 0) && (ip[2] == 0) && (ip[3] == 0)) {
        // Some strange situation where the DHCP renew probably has failed and erased the config.
        addLog(LOG_LEVEL_ERROR, F("WIFI : DHCP renew probably failed"));
        resetWiFi();
      }
    }
  } else {
    if (wifiStatus == ESPEASY_WIFI_SERVICES_INITIALIZED) { // SMY: WiFi seems to be disconnected and out of sync with internal state...
                                                           // strange... we should never get here!!
      addLog(LOG_LEVEL_ERROR, F("WIFI : WiFiCheck out of sync"));
      resetWiFi();
    }
  }

  if (wifiStatus != ESPEASY_WIFI_SERVICES_INITIALIZED) {
    if (timeOutReached(last_wifi_connect_attempt_moment + (1000 + wifi_connect_attempt * 200))) {
      WiFiConnectRelaxed();
    }
  }

  if (mqtt_reconnect_count > 10) {
    connectionCheckHandler();
  }
}

// ********************************************************************************
// Return subnet range of WiFi.
// ********************************************************************************
bool getSubnetRange(IPAddress& low, IPAddress& high)
{
  if (WifiIsAP(WiFi.getMode())) {
    // WiFi is active as accesspoint, do not check.
    return false;
  }

  if (wifiStatus < ESPEASY_WIFI_GOT_IP) {
    return false;
  }
  const IPAddress ip     = WiFi.localIP();
  const IPAddress subnet = WiFi.subnetMask();
  low  = ip;
  high = ip;

  // Compute subnet range.
  for (byte i = 0; i < 4; ++i) {
    if (subnet[i] != 255) {
      low[i]  = low[i] & subnet[i];
      high[i] = high[i] | ~subnet[i];
    }
  }
  return true;
}

String getLastDisconnectReason() {
  String reason = "(";

  reason += lastDisconnectReason;
  reason += F(") ");

  switch (lastDisconnectReason) {
    case WIFI_DISCONNECT_REASON_UNSPECIFIED:                reason += F("Unspecified");              break;
    case WIFI_DISCONNECT_REASON_AUTH_EXPIRE:                reason += F("Auth expire");              break;
    case WIFI_DISCONNECT_REASON_AUTH_LEAVE:                 reason += F("Auth leave");               break;
    case WIFI_DISCONNECT_REASON_ASSOC_EXPIRE:               reason += F("Assoc expire");             break;
    case WIFI_DISCONNECT_REASON_ASSOC_TOOMANY:              reason += F("Assoc toomany");            break;
    case WIFI_DISCONNECT_REASON_NOT_AUTHED:                 reason += F("Not authed");               break;
    case WIFI_DISCONNECT_REASON_NOT_ASSOCED:                reason += F("Not assoced");              break;
    case WIFI_DISCONNECT_REASON_ASSOC_LEAVE:                reason += F("Assoc leave");              break;
    case WIFI_DISCONNECT_REASON_ASSOC_NOT_AUTHED:           reason += F("Assoc not authed");         break;
    case WIFI_DISCONNECT_REASON_DISASSOC_PWRCAP_BAD:        reason += F("Disassoc pwrcap bad");      break;
    case WIFI_DISCONNECT_REASON_DISASSOC_SUPCHAN_BAD:       reason += F("Disassoc supchan bad");     break;
    case WIFI_DISCONNECT_REASON_IE_INVALID:                 reason += F("IE invalid");               break;
    case WIFI_DISCONNECT_REASON_MIC_FAILURE:                reason += F("Mic failure");              break;
    case WIFI_DISCONNECT_REASON_4WAY_HANDSHAKE_TIMEOUT:     reason += F("4way handshake timeout");   break;
    case WIFI_DISCONNECT_REASON_GROUP_KEY_UPDATE_TIMEOUT:   reason += F("Group key update timeout"); break;
    case WIFI_DISCONNECT_REASON_IE_IN_4WAY_DIFFERS:         reason += F("IE in 4way differs");       break;
    case WIFI_DISCONNECT_REASON_GROUP_CIPHER_INVALID:       reason += F("Group cipher invalid");     break;
    case WIFI_DISCONNECT_REASON_PAIRWISE_CIPHER_INVALID:    reason += F("Pairwise cipher invalid");  break;
    case WIFI_DISCONNECT_REASON_AKMP_INVALID:               reason += F("AKMP invalid");             break;
    case WIFI_DISCONNECT_REASON_UNSUPP_RSN_IE_VERSION:      reason += F("Unsupp RSN IE version");    break;
    case WIFI_DISCONNECT_REASON_INVALID_RSN_IE_CAP:         reason += F("Invalid RSN IE cap");       break;
    case WIFI_DISCONNECT_REASON_802_1X_AUTH_FAILED:         reason += F("802 1X auth failed");       break;
    case WIFI_DISCONNECT_REASON_CIPHER_SUITE_REJECTED:      reason += F("Cipher suite rejected");    break;
    case WIFI_DISCONNECT_REASON_BEACON_TIMEOUT:             reason += F("Beacon timeout");           break;
    case WIFI_DISCONNECT_REASON_NO_AP_FOUND:                reason += F("No AP found");              break;
    case WIFI_DISCONNECT_REASON_AUTH_FAIL:                  reason += F("Auth fail");                break;
    case WIFI_DISCONNECT_REASON_ASSOC_FAIL:                 reason += F("Assoc fail");               break;
    case WIFI_DISCONNECT_REASON_HANDSHAKE_TIMEOUT:          reason += F("Handshake timeout");        break;
    default:  reason                                               += getUnknownString();       break;
  }
  return reason;
}
