/*
ESP ZXY6005S
*/
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>						//EEPROM lib
#include <ESP8266HTTPClient.h>			//for httpGET
#include <Ticker.h>
#include <OneWire.h>          			//OneWire для DS18B20


/*******************************************************************/
extern "C" {
#include "user_interface.h"
extern struct rst_info resetInfo;
}
/*******************************************************************/

#define UPD

//firmware version
#define FW "0.02"

#define EEPROM_RESET_FLAG 1

#define DISFAVICON

#ifdef UPD
// #include <ESP8266HTTPUpdateS.h>		//UpdateServer
#include <ESP8266HTTPUpdateServer.h>	//UpdateServer
#endif

#define RESET_PIN 5						//порт для сброса настроек
#define _durationResetPull 6			//длительность нажатия кнопки для сброса настроек в секундах 
#define IP_SIZE 4						//октеты IP адресов
#define ETH_LEN 25						//размер команды EthAct после IP
#define ID_LEN 6						//размер ID
#define SSID_LEN 34						//размер SSID
#define PASS_LEN 64						//размер PASS


const uint16_t EA_SID = 0;						//0 - SID
const uint16_t EA_PAS = EA_SID + SSID_LEN;		//34 - PAS
const uint16_t EA_RESET = EA_PAS + PASS_LEN;	//флаг сброса настроек EEPROM
const uint16_t EA_ID = EA_RESET + 1;			//ID устройства
const uint16_t EA_NMOD = EA_ID + ID_LEN;		//режим работы точка, клиент, DHCP
const uint16_t EA_BAUD = EA_NMOD + 1;			//скорость шины UART 2400, 4800, 9600, 19200
const uint16_t EA_ADDR = EA_BAUD + 1;			//адрес шины UART
const uint16_t EA_MPT = EA_ADDR + 2;			//порт MQTT сервера
const uint16_t EA_SCR = EA_MPT + 2;				//скрипт сервера
const uint16_t EA_EIP = EA_SCR + ETH_LEN;       //ESP IP
const uint16_t EA_MSK = EA_EIP + IP_SIZE;       //маска
const uint16_t EA_GW = EA_MSK + IP_SIZE;        //шлюз
const uint16_t EA_DNS = EA_GW + IP_SIZE;        //DNS_1, нужен ли второй?
const uint16_t EA_ALL = EA_DNS + IP_SIZE;       //максимальный адрес EEPROM


//служебные переменные
boolean dhcp;           						//DHCP, on-off
#define debounceDelay 30      					//время ожидания установки состояния кнопки  

boolean rebootReq = false;      				//флаг для перезагрузки ESP
boolean resetFlag = false;      				//флаг для сброса настроек SSID
byte resetDurationCounter;      				//определяет длительность нажатия кнопки
unsigned long timeResetPoint;   				//точка подсчета секунд нажатия кнопки сброса
boolean apMode = false;							//флаг режима работы AP
boolean highMillis = false;						//флаг переполнения значения millis(); для подсчета uptime
byte rollOver = 0;          					//количество 50-ков дней при переполнении millis();



// конанды чтения данных:
// Aa - модель устройства,
// Aru - запрос напряжения,
// Ari - запрос тока,
// Arc - запрос состояния контроля (ток/напряжение)
// Aro - запрос состояния выхода.
// Ara - запрос потреблённой энергии AH

// команды управления
// Aso0/Aso1 - OUTPUT выкл/вкл
// AsuXXXXX - установка значения напряжения
// AsiXXXX - установка значения тока
// Asa0 - сброс значения AH

//переменные для ZXY
#define commandCount 12
#define addr "A"
#define xA 0		//0 - модель устройства
#define xSU 1		//1 - передать напряжение + значение
#define xSI 2		//2 - передать ток + значение
#define xSO 3		//3 - управление выходом + 1/0
#define xSA 4		//4 - сброс потреблённых AH
#define xRU 5		//5 - читать напряжение
#define xRI 6		//6 - читать ток
#define xRO 7		//7 - читать состояние выхода
#define xRC 8		//8 - читать контроль выхода
#define xRA 9		//9 - читать потреблённый AH
#define xRT 10		//10 - читать температуру

String cmdval[commandCount];			//масив команд ZXY
boolean cmdflag[commandCount];			//флаг передачи команды
String returnval[commandCount];			//буфер принятых данных от ZXY
String XML;
String AJAX;

uint8_t zxyBAUD;						//скорость UART
String zxyADDR;							//адрес UART

uint16_t curU;							//текущее значение напряжения
uint16_t curI;							//текущее значение тока
uint16_t curAH;							//текущее значение потреблённой энергии

float setU;
float setI;

unsigned long timeBusy; 				//период опроса
boolean busy;							//флаг занятости шины
uint8_t cmdnum;							//номер команды

// шапка HTML страницы
const char PAGE_Head[] PROGMEM = "<html><head><title>ZXY6000S</title></head><body><meta name=\"viewport\" content=\"width=device-width\">";

Ticker cronFlip;

ESP8266WebServer server(80);
#ifdef UPD
// ESP8266HTTPUpdateS httpUpdater;
ESP8266HTTPUpdateServer httpUpdater;
#endif

//This function will write String
//EEPROMWriteString(0, 12,"bla bla bla");
void EEPROMWriteString(uint16_t p_address, byte p_len, String p_value){
  byte len = p_value.length();
  if (len > p_len) return;
  EEPROM.write(p_address,len);
  len++;
  p_address++;
  unsigned char* buf = new unsigned char[len];
  p_value.getBytes(buf,len);
  for(byte i = 0; i < len; i++) {
    EEPROM.write(i + p_address, buf[i]);
  }
}

//String var = (const char*)EEPROMReadString(p_address, len, size, offset);
unsigned char* EEPROMReadString(uint16_t p_address, uint8_t len, uint8_t size, uint8_t offset){
  unsigned char* buf = new unsigned char[size];
  for(byte i = 0; i < len; i++) buf[i] = char(EEPROM.read(i + ((offset*size) + 1) + p_address));
  buf[len] = '\x0';
  return buf;
}

// EEPROMWriteIP(0, 192.168.0.X);
void EEPROMWriteIP(uint16_t p_address, String p_value) {      
  int8_t dot;         
  for (byte i = 0; i < 4; i++){
    dot = p_value.indexOf(".");
    if (dot) EEPROM.write(i + p_address, p_value.substring(0,dot).toInt());
    p_value = p_value.substring(dot + 1);
  }
}

// EEPROMWriteInt(0, 0xABCD); write a 2 byte integer to the eeprom at the specified address and address + 1
void EEPROMWriteInt(uint16_t p_address, uint16_t p_value) {
  byte lowByte = ((p_value >> 0) & 0xFF);
  byte highByte = ((p_value >> 8) & 0xFF);
  EEPROM.write(p_address, lowByte);
  EEPROM.write(p_address + 1, highByte);
}

// EEPROMReadInt(0); read a 2 byte integer from the eeprom at the specified address and address + 1
uint16_t EEPROMReadInt(uint16_t p_address) {
  byte lowByte = EEPROM.read(p_address);
  byte highByte = EEPROM.read(p_address + 1);
  return ((lowByte << 0) & 0xFF) + ((highByte << 8) & 0xFF00);
}

// This function will write a 4 byte float to the eeprom at the specified address and address + 3
void EEPROMWriteFloat(uint16_t p_address, float p_value){
  byte *x = (byte *)&p_value;
  for(byte i = 0; i < 4; i++) EEPROM.write(i+p_address, x[i]);
}

// This function will read a 4 byte float from the eeprom at the specified address and address + 3
float EEPROMReadFloat(uint16_t p_address){
  byte x[4];
  for(byte i = 0; i < 4; i++) x[i] = EEPROM.read(i+p_address);
  float *y = (float *)&x;
  return y[0];
}

/*********************************************************************/

void reqData(){
	if (cmdflag[cmdnum] == true){
		if (!busy){
			busy = true;
			timeBusy = millis();
			Serial.flush();
			Serial.println(cmdval[cmdnum]);
		}
		else if ((millis() - timeBusy) > (100)){
			String inString;
			while (Serial.available()) {
				char inChar = Serial.read();
				inString += inChar; 
				if (inChar == '\n') {
					returnval[cmdnum] = inString;
				}
			}
			busy = false;
			
			//отсекаем обработанные команды
			if (cmdnum && cmdnum < xSA) {
				// if (returnval[cmdnum] == cmdval[cmdnum]){
					cmdflag[cmdnum] = false;
				// }
			}
			if (cmdnum == xSA) {
				cmdflag[cmdnum] = false;
			}
			
			cmdnum++;
			// повторная попытка чтения информации об устройстве
			if (!returnval[xA].length()){
				cmdnum = xA;
			}
			else{
				cmdflag[xA] = false;
			}
			
			if (cmdnum < 9) {
				digitalWrite(2, LOW);
			}
			else if (cmdnum > 6){
				digitalWrite(2, HIGH);
			}
		}

	}
	else if (cmdnum >= commandCount){
		cmdnum = 0;
		// Serial.println(cmdnum + "   reset");
	}
	else{
		cmdnum++;
		// Serial.println(cmdnum + "   up");
	}
}

// отправка HTML страницы
void httpSend(String out){
	out+=F("</body></html>");
	server.send(200, "text/html", out);
}

// Парсинг полученных данных
void parsData(){
	curU = returnval[xRU].substring(3).toInt();
	curI = returnval[xRI].substring(3).toInt();
	curAH = returnval[xRA].substring(3).toInt();
}

// Обработчик главной страницы web сервера
void HTTP_Root(void) {
	String out, cmd;
	parsData();
	if(server.hasArg("su")){
		setU = server.arg("su").toFloat();
		uint16_t su  = 100 * setU;
		cmdval[xSU] = zxyADDR + "su";
		cmdval[xSU] += su;
		cmdflag[xSU] = true;
		// server.send(200, "text", "OK");
	}
	if(server.hasArg("si")){
		setI = server.arg("si").toFloat();
		uint16_t si  = 1000 * setI;
		cmdval[xSI] = zxyADDR + "si";
		cmdval[xSI] += si;
		cmdflag[xSI] = true;
		// server.send(200, "text", "OK");
	}
	if(server.hasArg("so")){
		uint8_t so  = server.arg("so").toInt();
		cmdval[xSO] = zxyADDR + "so";
		if (so == 2){
			so = !returnval[xRO].substring(3).toInt();
		}
		cmdval[xSO] += so;
		cmdflag[xSO] = true;
		// server.send(200, "text", "OK");

	}
		buildSCRIPT();
		out = AJAX;

		// out += "<form style=\"font-size: 20px;\">";
		out += "<br><form>";
		out += "<table border=\"0\" cellspacing=\"0\" width=\"260\"><tr><td align=\"center\" style=\"font-size: 20px;\">U=</td><td><input tabindex=\"1\" style=\"font-size: 20px; border: 1px solid; height: 30px; width: 100px; text-align: center;\" type=\"number\" name=\"su\" value=\"";
		out += setU;
		out += "\" oninput=\"up(this)\" step=\"0.01\" min=\"0\" max=\"60\"></td><td rowspan=\"2\"><input type=\"hidden\" name=\"so\" value=\"1\"><input tabindex=\"3\" style=\"font-size: 20px; height: 50px; width: 80px; text-align: center;\" type=\"submit\" value=\"SET\"></td></tr><tr>\n";
		out += "<td align=\"center\" style=\"font-size: 20px;\">I=</td><td><input tabindex=\"2\" style=\"font-size: 20px; border: 1px solid; height: 30px; width: 100px; text-align: center;\" type=\"number\" name=\"si\" value=\"";
		out += setI;
		out += "\" oninput=\"up(this)\" step=\"0.01\" min=\"0\" max=\"5\"></td></tr><t/able>";

		out += "</form>";
		httpSend(out);
	// }
}

// HTML
void buildSCRIPT(){
	AJAX="<html><head><SCRIPT>";
	AJAX+="var xmlHttp=createXmlHttpObject();\n";  
	AJAX+="function createXmlHttpObject(){\n"; 
	AJAX+="if(window.XMLHttpRequest){\n"; 
	AJAX+="xmlHttp=new XMLHttpRequest();\n"; 
	AJAX+="}else{\n"; 
	AJAX+="xmlHttp=new ActiveXObject('Microsoft.XMLHTTP');\n"; 
	AJAX+="}\n"; 
	AJAX+="return xmlHttp;\n"; 
	AJAX+="}\n"; 
	AJAX+="function process(){\n"; 
	AJAX+="if(xmlHttp.readyState==0 || xmlHttp.readyState==4){\n"; 
	AJAX+="xmlHttp.open('PUT','xml',true);\n"; 
	AJAX+="xmlHttp.onreadystatechange=handleServerResponse;\n"; 
	AJAX+="xmlHttp.send(null);\n"; 
	AJAX+="}\n"; 
	AJAX+="setTimeout('process()',1000);\n"; 
	AJAX+="}\n"; 
	AJAX+="function handleServerResponse(){\n"; 
	AJAX+="if(xmlHttp.readyState==4 && xmlHttp.status==200){\n"; 
	AJAX+="xmlResponse=xmlHttp.responseXML;\n"; 
	AJAX+="xmldoc = xmlResponse.getElementsByTagName('u');\n"; 
	AJAX+="message = xmldoc[0].firstChild.nodeValue;\n"; 
	AJAX+="document.getElementById('uid').innerHTML=message;\n";
	AJAX+="xmldoc = xmlResponse.getElementsByTagName('i');\n"; 
	AJAX+="message = xmldoc[0].firstChild.nodeValue;\n"; 
	AJAX+="document.getElementById('iid').innerHTML=message;\n";
	//power
	AJAX+="xmldoc = xmlResponse.getElementsByTagName('w');\n"; 
	AJAX+="message = xmldoc[0].firstChild.nodeValue;\n"; 
	AJAX+="document.getElementById('wid').innerHTML=message;\n";
	//AH
	AJAX+="xmldoc = xmlResponse.getElementsByTagName('ah');\n"; 
	AJAX+="message = xmldoc[0].firstChild.nodeValue;\n"; 
	AJAX+="document.getElementById('ahid').innerHTML=message;\n";
	//CC
	AJAX+="xmldoc = xmlResponse.getElementsByTagName('cc');\n"; 
	AJAX+="message = xmldoc[0].firstChild.nodeValue;\n"; 
	AJAX+="document.getElementById('ccid').innerHTML=message;\n";
	AJAX+="xmldoc = xmlResponse.getElementsByTagName('out');\n"; 
	AJAX+="message = xmldoc[0].firstChild.nodeValue;\n"; 
	AJAX+="document.getElementById('outid').value=message;\n"; 
	AJAX+="}\n"; 
	AJAX+="}\n";
	AJAX+="function so(key){\n";
	AJAX+="server = \"/zxy?so=\" + key;\n";
	AJAX+="nocache = \"&nocache=\"+ Math.random() * 1000000;\n";   // чтобы браузер не обращался к кэш
	AJAX+="var xmlhttp;\n";
	AJAX+="if (window.XMLHttpRequest){;\n";							// код для IE7+, Firefox, Chrome, Opera, Safari
	AJAX+="xmlhttp=new XMLHttpRequest();\n";
	AJAX+="} else {\n";												// код для IE6, IE5
	AJAX+="xmlhttp=new ActiveXObject(\"Microsoft.XMLHTTP\");\n";
	AJAX+="}\n";
	AJAX+="xmlhttp.onreadystatechange=function(){\n";
	AJAX+="if (xmlhttp.readyState==4 && xmlhttp.status==200){\n";
	AJAX+="process(xmlhttp.responseText);\n";
	AJAX+="}\n";
	AJAX+="}\n";
    AJAX+="xmlhttp.open(\"GET\", server, true);\n";
    AJAX+="xmlhttp.send();\n";
	AJAX+="}\n";
	AJAX+="function up(e) {\n";
	AJAX+="if (e.value.indexOf(\".\") != '-1') {\n";
	AJAX+="e.value=e.value.substring(0, e.value.indexOf(\".\") + 3);\n";
	AJAX+="}\n";
	AJAX+="}\n";
	AJAX+="</SCRIPT>\n"; 
	AJAX+="</head><title>ZXY6000S</title>\n";
	AJAX+="<BODY onload=\"process();\"><meta name=\"viewport\" content=\"width=device-width\">\n";
	AJAX+=F("ESP ZXY6000S (fw: "FW")<br><a href=\"/zxy/cfg/\">Config</a><br><br>\n");
	AJAX+=F("<table border=\"1\" cellspacing=\"0\" width=\"260\"><tr align=\"center\"><td>VOLTAGE</td><td>CURRENT</td><td>OUTPUT</td></tr><tr style=\"font-size: 30px; text-align: center;\">\n");
	AJAX+="<td><div id=\"uid\">U</div></td>\n";
	AJAX+="<td><div id=\"iid\">I</div></td>\n";
	AJAX+="<td><div id=\"ccid\">OFF</div></td></tr>\n";
	AJAX+="<tr style=\"font-size: 25px; text-align: center;\"><td colspan=\"2\"><div id=\"wid\"></div></td><td rowspan=\"2\"><input tabindex=\"-1\" type=\"submit\" id=\"outid\" name=\"state\" value=\"OFF\" onclick=\"so(2);\" style=\"font-size: 20px; border: 0px solid; height: 60px; width: 80px; text-align: center;\"></td></tr>\n";
	AJAX+="<tr style=\"font-size: 25px; text-align: center;\"><td colspan=\"2\"><div id=\"ahid\"></div></td><tr>\n";
	AJAX+="</table>\n";
}

void buildXML(){
	parsData();
	
	XML="<?xml version='1.0'?><data>";
    XML+="<u>";
    XML+=curU * 0.01;
    XML+="</u>";
    XML+="<i>";
    XML+=curI * 0.001;
    XML+="</i>";
	XML+="<w>";
    XML+=curI * 0.001 * curU * 0.01;
    XML+=" W</w>";
	XML+="<ah>";
	if (curAH > 1000){
		XML+=curAH * 0.001;
		XML+=" AH";
	}
	else {
		XML+=curAH;
		XML+=" mAH";
	}
	XML+="</ah>";
    XML+="<cc>";
	if (returnval[xRO].substring(3).toInt() == 0){
		XML+=F("OFF");
	}
	else if (returnval[xRC].substring(3).toInt() == 1){
		XML+=F("CV");
	}
	else{
		XML+=F("CC");
	}
    XML+="</cc>";
	XML+="<out>";
	if (returnval[xRO].substring(3).toInt() == 0){
		XML+=F("ON");
	}
	else {
		XML+=F("OFF");
	}
	XML+="</out>";
	XML+="</data>"; 
}


void handleXML(){
	buildXML();
	server.send(200,"text/xml",XML);
}


void HTTP_SetUp(void) {
	String out, ssid_ap, pass_ap;
	out = FPSTR(PAGE_Head);
	out+=F("<a href=\"/zxy\">Back</a><br>SSID: ");
	if (!apMode){
		out+=WiFi.SSID();
		out+=F(" RSSI: ");
		out+=WiFi.RSSI();
		out+=F(" dBm");
	}
	else{
		out+=F("ESPap");
	}
	out+=F("<br>-- Config --<br><a href=\"/zxy/cfg/?cn=0\">Wi-Fi</a>  |  <a href=\"/zxy/cfg/?cn=1\">IP</a> | <a href=\"/zxy/cfg/?cn=2\">MOD</a>  |  <a href=\"/zxy/cfg/?cn=4\">RES</a> |  <a href=\"/zxy/cfg/?cn=5\">INF</a><br>-<br>");
	if (server.hasArg("pin")){
		if (server.arg("pin").toInt() == 123){
			EEPROM.begin(EA_ALL);
			switch (server.arg("res").toInt()) {
				case 1:
					EEPROM.write(EA_SID,255);
					for (uint16_t i = EA_BAUD; i < EA_ALL; i++) EEPROM.write(i,0);
					EEPROM.commit();
					rebootReq = true;
				break;
				case 2:
					for (uint16_t i = EA_BAUD; i < EA_ALL; i++) EEPROM.write(i,0);
				case 255:
					rebootReq = true;
				break;
			}
			if(server.hasArg("sid")){
				ssid_ap = server.arg("sid");
				pass_ap = server.arg("pas");
				if(ssid_ap != ""){
					EEPROMWriteString(EA_SID, SSID_LEN, ssid_ap);
					EEPROMWriteString(EA_PAS, PASS_LEN, pass_ap);
					rebootReq = true;
				}
			}
			if(server.hasArg("br")) {
				zxyBAUD = server.arg("br").toInt();
				EEPROM.write(EA_BAUD, zxyBAUD);
				rebootReq = true;
			}
			if(server.hasArg("aa")){
				zxyADDR = server.arg("aa");
				if(zxyADDR != ""){
					EEPROMWriteString(EA_ADDR, 1, zxyADDR);
					rebootReq = true;
				}
			}
			int8_t dot;

			if(server.hasArg("nm")){
				byte nm = server.arg("nm").toInt();
				if (nm == 1){
					EEPROM.write(EA_NMOD, nm);
					if(server.hasArg("eip")) EEPROMWriteIP(EA_EIP, server.arg("eip"));
					if(server.hasArg("msk")) EEPROMWriteIP(EA_MSK, server.arg("msk"));
					if(server.hasArg("gw")) EEPROMWriteIP(EA_GW, server.arg("gw"));
				}
				else{
					EEPROM.write(EA_NMOD, 255);
				}
				rebootReq = true;
			}
			out+=F("Config OK!");
			if(rebootReq) {
				out+=F("<br>ESP restarted!");
			}
			EEPROM.commit();
			EEPROM.end();
		}
	}
	else {
		if(server.hasArg("cn")){
			byte cn = server.arg("cn").toInt();
			if (cn!=5) out+=F("<form action=\"/zxy/cfg/\">PIN: <input name=\"pin\" size=\"3\" maxlength=\"3\" required><br>");
			switch (cn) {
				case 0:
					out+=F("SSiD: <input name=\"sid\" required placeholder=\"");
					out+=WiFi.SSID();
					out+=F("\"><br>PaSS: <input type=\"password\" name=\"pas\"");
					if (!apMode){
						out+=F(" placeholder=\"******\"");
					}
					out+=">";
				break;
				case 1:
					out+=F("Ip cfg: <select name=\"nm\"><option value=\"1\"");
					if (!dhcp) out+=F("selected");
					out+=F(">Static</option><option value=\"255\"");
					if (dhcp) out+=F("selected");
					out+=F(">DHCP</option></select><br>EspIp: <input name=\"eip\" value=\"");
					out+=String(WiFi.localIP().toString());
					out+=F("\"><br>Mask: <input name=\"msk\" value=\"");
					out+=String(WiFi.subnetMask().toString());
					out+=F("\"><br>DGW: <input name=\"gw\" value=\"");
					out+=String(WiFi.gatewayIP().toString());
					out+=F("\">");          
				break;
				case 2:
					out+=F("BAUD: <select name=\"br\"><option value=\"0\"");
					if (zxyBAUD == 0) out+=F("selected");
					out+=F(">19200</option><option value=\"1\"");
					if (zxyBAUD == 1) out+=F("selected");
					out+=F(">9600</option><option value=\"2\"");
					if (zxyBAUD == 2) out+=F("selected");
					out+=F(">4800</option><option value=\"3\"");
					if (zxyBAUD == 3) out+=F("selected");
					out+=F(">2400</option></select><br>ADDR: <input size=\"4\" maxlength=\"1\" name=\"aa\" value=\"");
					out+=zxyADDR;
					out+=F("\">");                  
				break; 
				case 4:
					for (byte i = 1; i < 4; i++){
						out+=F("<input name=\"res\" type=\"radio\" value=\"");
						switch (i) {
							case 1:   out+=F("1\"> Reset All");   break;
							case 2:   out+=F("3\"> Reset IP");  break;
							default:  out+=F("255\"> Restart"); break;
						}
						out+=F("<br>");
					}
					out+=F("<input type=\"submit\" value=\"OK\"></form>");
				break;
				case 5:
					char s[16];
					long secsUp = millis()/1000;
					sprintf(s, "Up: %dd %02d:%02d:%02d", (rollOver*50)+(secsUp/86400), (secsUp/3600)%24, (secsUp/60)%60, secsUp%60);
					out+=s;
					out+=F("<br>");
					out+=ESP.getResetReason();
          
					uint32_t realSize = ESP.getFlashChipRealSize();
					uint32_t ideSize = ESP.getFlashChipSize();
					FlashMode_t ideMode = ESP.getFlashChipMode();

					out+=F("<br>Config ");
					if(ideSize == realSize) {
						out+=F("ok");
						#ifdef UPD
							if (realSize > 4000000) out+=F(" - <a href=\"/zxy/upd\">fwUpd</a>");
						#endif
					} else {
						out+=F("wrong!");
					}
					out+=F("<br>Fl. id: ");
					out+=ESP.getFlashChipId();
					out+=F("<br>Fl. si: ");
					out+=realSize; //4194304
					out+=F("<br>Id. si: ");
					out+=ideSize;
					out+=F("<br>Id. sp: ");
					out+=ESP.getFlashChipSpeed();
					out+=F("<br>Id. m: ");
					switch (ideMode) {
						case FM_QIO:
							out+=F("QIO");
						break;
						case FM_QOUT:
							out+=F("QOUT");
						break;
						case FM_DIO:
							out+=F("DIO");
						break;
						case FM_DOUT:
							out+=F("DOUT");
						break;
						default:
							out+=F("UNKN");
						break;
					}	
					out+=F("<br>SDK: ");
					out+=ESP.getSdkVersion();
					out+=F("<br>EE: ");
					out+=EA_ALL;
					out+=F("<br>-<br>model: ");
					out+=returnval[xA];
					out+=F("<br>raw sU: ");
					out+=returnval[xSU];
					out+=F("<br>raw sI: ");
					out+=returnval[xSI];
					out+=F("<br>raw sO: ");
					out+=returnval[xSO];
					out+=F("<br>raw sA: ");
					out+=returnval[xSA];
					out+=F("<br>raw rU: ");
					out+=returnval[xRU];
					out+=F("<br>raw rI: ");
					out+=returnval[xRI];
					out+=F("<br>raw rO: ");
					out+=returnval[xRO];
					out+=F("<br>raw rC: ");
					out+=returnval[xRC];
					out+=F("<br>raw rA: ");
					out+=returnval[xRA];
					out+=F("<br>raw rT: ");
					out+=returnval[xRT];
					out+=F("<br>-<br><a href=\"https://money.yandex.ru/to/41001806160801\">donate</a>");
				break;
			}
			if (cn<4) out+=F("<br><input type=\"submit\" value=\"Save\"></form>");
		}
	}
	httpSend(out);
}

// обработчик запроса на левую страницу
void HTTP_NF(void) {
	String out = FPSTR(PAGE_Head);
	out+=F("Try http://ip/zxy");
	httpSend(out);
}

#ifdef DISFAVICON
void HTTP_Favicon(void){
	server.send (404);
}
#endif

// Режим SoftAP
void WiFiSAP(void){
	const char *ssid_ap = "ZXY60xx";
	WiFi.mode(WIFI_AP);
	WiFi.softAP(ssid_ap);
	// WiFi.softAP("ESPap","megaesp123");
	delay(2000);
	server.on("/zxy", HTTP_Root);
	server.on("/zxy/cfg/", HTTP_SetUp);
	server.onNotFound(HTTP_SetUp);
	server.on("/xml", handleXML);
	#ifdef DISFAVICON
	server.on("/favicon.ico", HTTP_Favicon);
	#endif
	#ifdef UPD
	httpUpdater.setup(&server, "/zxy/upd");
	#endif
	server.begin();
}

void cron(){
	returnval[xA] = "";
	cmdflag[xA] = true;
}

void setup(void) {
	pinMode(2, OUTPUT);

	EEPROM.begin(EA_ALL);

	byte resetEPROM = EEPROM.read(EA_RESET);
	if (resetEPROM != EEPROM_RESET_FLAG){
		EEPROM.write(EA_RESET,EEPROM_RESET_FLAG);
		for (uint16_t i = 0; i < EA_ALL; i++) EEPROM.write(i,0);
		EEPROM.commit();
	}
  
	byte nmod = EEPROM.read(EA_NMOD);
	if (nmod && nmod != 255){
		dhcp = false;
	}
	else {
		dhcp = true;
	}
	
	zxyBAUD = EEPROM.read(EA_BAUD);
	switch (zxyBAUD) {
		case 0:
		Serial.begin(19200);
		break;
		case 1:
		Serial.begin(9600);;
		break;
		case 2:
		Serial.begin(4800);
		break;
		case 3:
		Serial.begin(2400);
		break;
		default:
		Serial.begin(9600);
		zxyBAUD = 1;
		EEPROM.write(EA_BAUD,zxyBAUD);
		EEPROM.commit();
		break;
	}
	
	byte len_ee = EEPROM.read(EA_ADDR);
	if (len_ee && len_ee < 2) {
		zxyADDR = (const char*)EEPROMReadString(EA_ADDR, len_ee, 2, 0);
	}
	else{
		zxyADDR = "A";
	}
	// ={"a", addr"su", addr"si", addr"so", addr"sa0", addr"ru", addr"ri", addr"ro", addr"rc", addr"ra", addr"rt"}
	cmdval[xA] = zxyADDR + "a";
	cmdflag[xA] = true;
	cmdval[xSU] = zxyADDR + "su";
	cmdflag[xSU] = false;
	cmdval[xSI] = zxyADDR + "si";
	cmdflag[xSI] = false;
	cmdval[xSO] = zxyADDR + "so";
	cmdflag[xSO] = false;
	cmdval[xSA] = zxyADDR + "sa0";
	cmdflag[xSA] = false;
	cmdval[xRU] = zxyADDR + "ru";
	cmdflag[xRU] = true;
	cmdval[xRI] = zxyADDR + "ri";
	cmdflag[xRI] = true;
	cmdval[xRO] = zxyADDR + "ro";
	cmdflag[xRO] = true;
	cmdval[xRC] = zxyADDR + "rc";
	cmdflag[xRC] = true;
	cmdval[xRA] = zxyADDR + "ra";
	cmdflag[xRA] = true;
	cmdval[xRT] = zxyADDR + "rt";
	cmdflag[xRT] = true;
	
// #define xA 0		//0 - модель устройства
// #define xSU 1		//1 - передать напряжение + значение
// #define xSI 2		//2 - передать ток + значение
// #define xSO 3		//3 - управление выходом + 1/0
// #define xSA 4		//4 - сброс потреблённых AH
// #define xRU 5		//5 - читать напряжение
// #define xRI 6		//6 - читать ток
// #define xRO 7		//7 - читать состояние выхода
// #define xRC 8		//8 - читать контроль выхода
// #define xRA 9		//9 - читать потреблённый AH
// #define xRT 10		//10 - читать температуру
	
	
	len_ee = EEPROM.read(EA_SID);
	if(len_ee && len_ee < 33){
		WiFi.mode(WIFI_STA);                // Режим STATION
		const char *ssid  = (const char*)EEPROMReadString(EA_SID, len_ee, 33, 0);
		len_ee = EEPROM.read(EA_PAS);
		if(len_ee > PASS_LEN) len_ee = 0;
		const char *password  = (const char*)EEPROMReadString(EA_PAS, len_ee, PASS_LEN, 0);
		WiFi.begin(ssid, password);             // запуск Wi-Fi клиента
		if (!dhcp){
			IPAddress ip, gateway, subnet;
			for (byte i = 0; i < IP_SIZE; i++){
				ip[i] = EEPROM.read(i + EA_EIP);
				gateway[i] = EEPROM.read(i + EA_GW);
				subnet[i] = EEPROM.read(i + EA_MSK);
			}
			WiFi.config(ip, gateway, subnet);
		}
		delay(1000);
		byte wStat = 0;
		while (WiFi.status() != WL_CONNECTED) {
			delay(1000);
			wStat++;
			if (wStat > 15) break;
		}
    if (wStat < 15 && WiFi.waitForConnectResult() == WL_CONNECTED){
      server.on("/zxy/cfg/", HTTP_SetUp);
      server.on("/zxy", HTTP_Root);
      server.on("/xml", handleXML);
      #ifdef DISFAVICON
      server.on("/favicon.ico", HTTP_Favicon);
      #endif
      server.onNotFound(HTTP_NF);
      #ifdef UPD
      httpUpdater.setup(&server, "/zxy/upd");
      #endif
      server.begin();


    }
	else {  //Режим SoftAP
		WiFiSAP();
		apMode = true;
			}
		}
		else {  //Режим SoftAP
		WiFiSAP();
		apMode = true;
	}
	EEPROM.end();
	
	cronFlip.attach(60, cron);
}



void loop() {
	server.handleClient();
	reqData();
  
	if (apMode && millis() > 300000) rebootReq = true;
	if (millis() >= 3000000000) highMillis = true;
	if (millis() <= 100000 && highMillis){
		rollOver++;
		highMillis = false;
	}
	if (rebootReq){
		delay(3000);
		ESP.reset();
	}
}

