# JSON A SALIDA



## DESCRIPCION PINES PLACA COM ELRON
- IO23	LED FALLA
- IO22	LED COMUNICACION
- IO21	LED ALARMA
- IO19	LED DC
- IO5		ENTRADA OPTO 4
- IO17	ENTRADA OPTO 3
- IO16	ENTRADA OPTO 2
- IO4		ENTRADA OPTO 1
- IO0		LED BATERIA
- IO2 	LED ON
- IO10	TX
- IO9		RX
- IO13	ENABLE 485
- IO12	RELE_BATERIA
- IO39	SUPERVISION BATERIA ADC
- IO36	SUPERVISION TENSION ENTRADA



- IO18 Sirve para entrar en modo configuración por UART.
  Permite setear el SSID y la password.
  Después continuar la configuración desde la página web http://x.x.x.x:80

Configuracion base: Remplazar la ip por la de la central.
Configuración actual:
{
  "intervalo": 1500,
  "etiquetas": [
    {
      "etiqueta": "ALARMA",
      "gpio": 17,
      "objeto": "barstatus"
    },
    {
      "etiqueta": "FALLA",
      "gpio": 32,
      "objeto": "barstatus"
    },
    {
      "etiqueta": "DESCONEXION",
      "gpio": 27,
      "objeto": "barstatus"
    },
    {
      "etiqueta": "TEST",
      "gpio": 33,
      "objeto": "barstatus"
    }
  ],
  "urls": [
    "http://192.168.88.196/api/barstatus"
  ],
  "entradas": [
    {
      "gpio": 17,
      "post_url": "http://192.168.88.196/api/cmd",
      "post_json": "{\"cmdReset\":{}}",
      "trigger_level": 0
    },
    {
      "gpio": 5,
      "post_url": "http://192.168.88.196/api/cmd",
      "post_json": "{\"cmdACK\":{}}",
      "trigger_level": 0
    },
    {
      "gpio": 4,
      "post_url": "http://192.168.88.196/api/cmd",
      "post_json": "{\"cmdACK\":{}}",
      "trigger_level": 0
    },
    {
      "gpio": 16,
      "post_url": "http://192.168.88.196/api/cmd",
      "post_json": "{\"cmdACK\":{}}",
      "trigger_level": 0
    }
  ]
}